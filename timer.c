#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <signal.h>
#include <time.h>
#include <lua.h>
#include <lauxlib.h>

#include "php.h"
#include "php_luasandbox.h"
#include "luasandbox_timer.h"

char luasandbox_timeout_message[] = "The maximum execution time for this script was exceeded";

#ifdef LUASANDBOX_NO_CLOCK

void luasandbox_timer_install_handler(struct sigaction * oldact) {}
void luasandbox_timer_remove_handler(struct sigaction * oldact) {}
void luasandbox_timer_create(luasandbox_timer_set * lts,
		php_luasandbox_obj * sandbox) {}
void luasandbox_timer_set_limits(luasandbox_timer_set * lts,
		struct timespec * normal_timeout, 
		struct timespec * emergency_timeout) {}
void luasandbox_timer_start(luasandbox_timer_set * lts) {}
void luasandbox_timer_stop(luasandbox_timer_set * lts) {}

void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts) {
	ts->tv_sec = ts->tv_nsec = 0;
}
void luasandbox_timer_timeout_error(lua_State *L) {}
int luasandbox_is_expired(luasandbox_timer_set * lts) {
	return 0;
}


#else

static void luasandbox_timer_handle(int signo, siginfo_t * info, void * context);
static void luasandbox_timer_create_one(luasandbox_timer * lt, php_luasandbox_obj * sandbox, 
		int emergency);
static void luasandbox_timer_timeout_hook(lua_State *L, lua_Debug *ar);
static void luasandbox_timer_set_one_time(luasandbox_timer * lt, struct timespec * ts);
static void luasandbox_timer_stop_one(luasandbox_timer * lt, struct timespec * remaining);
static void luasandbox_update_usage(luasandbox_timer_set * lts);

static inline void luasandbox_timer_zero(struct timespec * ts)
{
	ts->tv_sec = ts->tv_nsec = 0;
}

static inline int luasandbox_timer_is_zero(struct timespec * ts)
{
	return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

static inline void luasandbox_timer_subtract(
		struct timespec * a, const struct timespec * b)
{
	a->tv_sec -= b->tv_sec;
	if (a->tv_nsec < b->tv_nsec) {
		a->tv_sec--;
		a->tv_nsec += 1000000000L - b->tv_nsec;
	} else {
		a->tv_nsec -= b->tv_nsec;
	}
}

static inline void luasandbox_timer_add(
		struct timespec * a, const struct timespec * b)
{
	a->tv_sec += b->tv_sec;
	a->tv_nsec += b->tv_nsec;
	if (a->tv_nsec > 1000000000L) {
		a->tv_nsec -= 1000000000L;
		a->tv_sec++;
	}
}

void luasandbox_timer_install_handler(struct sigaction * oldact)
{
	struct sigaction newact;
	newact.sa_sigaction = luasandbox_timer_handle;
	newact.sa_flags = SA_SIGINFO;
	sigprocmask(SIG_BLOCK, NULL, &newact.sa_mask);
	sigaction(LUASANDBOX_SIGNAL, &newact, oldact);
}

void luasandbox_timer_remove_handler(struct sigaction * oldact)
{
	sigaction(LUASANDBOX_SIGNAL, oldact, NULL);
}

static void luasandbox_timer_handle(int signo, siginfo_t * info, void * context)
{
	luasandbox_timer_callback_data * data;
	lua_State * L;
	
	if (signo != LUASANDBOX_SIGNAL
			|| info->si_code != SI_TIMER
			|| !info->si_value.sival_ptr)
	{
		return;
	}

	data = (luasandbox_timer_callback_data*)info->si_value.sival_ptr;
	data->sandbox->timed_out = 1;
	L = data->sandbox->state;
	if (data->emergency) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		data->sandbox->emergency_timed_out = 1;
		if (data->sandbox->in_php) {
			// The whole PHP request context is dirty now. We need to kill it,
			// like what happens if there is a max_execution_time timeout.
			zend_error(E_ERROR, "The maximum execution time for a Lua sandbox script "
					"was exceeded and a PHP callback failed to return");
		} else {
			// The Lua state is dirty now and can't be used again.
			lua_pushstring(L, "emergency timeout");
			luasandbox_wrap_fatal(L);
			lua_error(L);
		}
	} else {
		// Set a hook which will terminate the script execution in a graceful way
		lua_sethook(L, luasandbox_timer_timeout_hook,
			LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
	}
}

static void luasandbox_timer_timeout_hook(lua_State *L, lua_Debug *ar)
{
	// Avoid infinite recursion
	lua_sethook(L, luasandbox_timer_timeout_hook, 0, 0);
	// Do a longjmp to report the timeout error
	luasandbox_timer_timeout_error(L);
}

void luasandbox_timer_timeout_error(lua_State *L)
{
	lua_pushstring(L, luasandbox_timeout_message);
	luasandbox_wrap_fatal(L);
	lua_error(L);
}

void luasandbox_timer_create(luasandbox_timer_set * lts, php_luasandbox_obj * sandbox)
{
	luasandbox_timer_zero(&lts->usage);
	luasandbox_timer_zero(&lts->normal_limit);
	luasandbox_timer_zero(&lts->normal_remaining);
	luasandbox_timer_zero(&lts->emergency_limit);
	luasandbox_timer_zero(&lts->emergency_remaining);
	lts->is_running = 0;
	lts->normal_running = 0;
	lts->emergency_running = 0;
	lts->sandbox = sandbox;
}

void luasandbox_timer_set_limits(luasandbox_timer_set * lts,
		struct timespec * normal_timeout, 
		struct timespec * emergency_timeout)
{
	int was_running = 0;
	if (lts->is_running) {
		was_running = 1;
		lusandbox_timer_stop(lts);
	}
	lts->normal_remaining = lts->normal_limit = *normal_timeout;
	lts->emergency_remaining = lts->emergency_limit = *emergency_timeout;
	if (was_running) {
		luasandbox_timer_start(lts);
	}
}

void luasandbox_timer_start(luasandbox_timer_set * lts)
{
	if (lts->is_running) {
		// Already running
		return;
	}
	lts->is_running = 1;
	// Initialise usage timer
	clock_gettime(LUASANDBOX_CLOCK_ID, &lts->usage_start);

	// Create normal timer if requested
	if (!luasandbox_timer_is_zero(&lts->normal_remaining)) {
		lts->normal_running = 1;
		luasandbox_timer_create_one(&lts->normal_timer, lts->sandbox, 0);
		luasandbox_timer_set_one_time(&lts->normal_timer, &lts->normal_remaining);
	} else {
		lts->normal_running = 0;
	}
	// Create emergency timer if requested
	if (!luasandbox_timer_is_zero(&lts->emergency_remaining)) {
		lts->emergency_running = 1;
		luasandbox_timer_create_one(&lts->emergency_timer, lts->sandbox, 1);
		luasandbox_timer_set_one_time(&lts->emergency_timer, &lts->emergency_remaining);
	} else {
		lts->emergency_running = 0;
	}
}

static void luasandbox_timer_create_one(luasandbox_timer * lt, php_luasandbox_obj * sandbox, 
		int emergency)
{
	struct sigevent ev;

	// Make valgrind happy
	memset(&ev, 0, sizeof(ev));

	lt->cbdata.emergency = emergency;
	lt->cbdata.sandbox = sandbox;
	
	ev.sigev_notify = SIGEV_SIGNAL;
	ev.sigev_signo = LUASANDBOX_SIGNAL;
	ev.sigev_value.sival_ptr = (void*)&lt->cbdata;

	timer_create(LUASANDBOX_CLOCK_ID, &ev, &lt->timer);
}

static void luasandbox_timer_set_one_time(luasandbox_timer * lt, struct timespec * ts)
{
	struct itimerspec its;
	luasandbox_timer_zero(&its.it_interval);
	its.it_value = *ts;
	timer_settime(lt->timer, 0, &its, NULL);
}


void luasandbox_timer_stop(luasandbox_timer_set * lts)
{
	struct timespec usage;

	if (lts->is_running) {
		lts->is_running = 0;
	} else {
		return;
	}

	// Stop the interval timers and save the time remaining
	if (lts->emergency_running) {
		luasandbox_timer_stop_one(&lts->emergency_timer, &lts->emergency_remaining);
		lts->emergency_running = 0;
	}
	if (lts->normal_running) {
		luasandbox_timer_stop_one(&lts->normal_timer, &lts->normal_remaining);
		lts->normal_running = 0;
	}

	// Update the usage
	luasandbox_update_usage(lts);
	clock_gettime(LUASANDBOX_CLOCK_ID, &usage);
	luasandbox_timer_subtract(&usage, &lts->usage_start);
	luasandbox_timer_add(&lts->usage, &usage);
}


static void luasandbox_timer_stop_one(luasandbox_timer * lt, struct timespec * remaining)
{
	static struct timespec zero = {0, 0};
	struct itimerspec its;
	timer_gettime(lt->timer, &its);
	remaining->tv_sec = its.it_value.tv_sec;
	remaining->tv_nsec = its.it_value.tv_nsec;
	luasandbox_timer_set_one_time(lt, &zero);
	timer_delete(lt->timer);
}

void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts)
{
	if (lts->is_running) {
		luasandbox_update_usage(lts);
	}
	*ts = lts->usage;
}

int luasandbox_timer_is_expired(luasandbox_timer_set * lts)
{
	if (!luasandbox_timer_is_zero(&lts->normal_limit)) {
		if (luasandbox_timer_is_zero(&lts->normal_remaining)) {
			return 1;
		}
	}
	if (!luasandbox_timer_is_zero(&lts->emergency_limit)) {
		if (luasandbox_timer_is_zero(&lts->emergency_remaining)) {
			return 1;
		}
	}
	return 0;
}

static void luasandbox_update_usage(luasandbox_timer_set * lts)
{
	struct timespec current, usage;
	clock_gettime(LUASANDBOX_CLOCK_ID, &current);
	usage = current;
	luasandbox_timer_subtract(&usage, &lts->usage_start);
	luasandbox_timer_add(&lts->usage, &usage);
	lts->usage_start = current;
}
#endif
