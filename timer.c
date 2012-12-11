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
void luasandbox_timer_enable_profiler(luasandbox_timer_set * lts, struct timespec * period) {}
void luasandbox_timer_start(luasandbox_timer_set * lts) {}
void luasandbox_timer_stop(luasandbox_timer_set * lts) {}
void luasandbox_timer_destroy(luasandbox_timer_set * lts) {}

void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts) {
	ts->tv_sec = ts->tv_nsec = 0;
}
void luasandbox_timer_timeout_error(lua_State *L) {}
int luasandbox_is_expired(luasandbox_timer_set * lts) {
	return 0;
}


#else

enum {
	LUASANDBOX_TIMER_NORMAL,
	LUASANDBOX_TIMER_EMERGENCY,
	LUASANDBOX_TIMER_PROFILER
};

static void luasandbox_timer_handle_signal(int signo, siginfo_t * info, void * context);
static void luasandbox_timer_handle_profiler(union sigval sv);
static void luasandbox_timer_create_one(luasandbox_timer * lt, php_luasandbox_obj * sandbox, 
		int type);
static void luasandbox_timer_timeout_hook(lua_State *L, lua_Debug *ar);
static void luasandbox_timer_profiler_hook(lua_State *L, lua_Debug *ar);
static void luasandbox_timer_set_one_time(luasandbox_timer * lt, struct timespec * ts);
static void luasandbox_timer_set_periodic(luasandbox_timer * lt, struct timespec * period);
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
	newact.sa_sigaction = luasandbox_timer_handle_signal;
	newact.sa_flags = SA_SIGINFO;
	sigprocmask(SIG_BLOCK, NULL, &newact.sa_mask);
	sigaction(LUASANDBOX_SIGNAL, &newact, oldact);
}

void luasandbox_timer_remove_handler(struct sigaction * oldact)
{
	sigaction(LUASANDBOX_SIGNAL, oldact, NULL);
}

static void luasandbox_timer_handle_signal(int signo, siginfo_t * info, void * context)
{
	luasandbox_timer_callback_data * data;
	
	if (signo != LUASANDBOX_SIGNAL
			|| info->si_code != SI_TIMER
			|| !info->si_value.sival_ptr)
	{
		return;
	}

	data = (luasandbox_timer_callback_data*)info->si_value.sival_ptr;

	if (data->type == LUASANDBOX_TIMER_PROFILER) {
		luasandbox_timer_handle_profiler(info->si_value);
	} else {
		lua_State * L = data->sandbox->state;
		data->sandbox->timed_out = 1;

		if (data->type == LUASANDBOX_TIMER_EMERGENCY) {
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
				LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
		}
	}
}

static void luasandbox_timer_handle_profiler(union sigval sv)
{
	luasandbox_timer_callback_data * data = (luasandbox_timer_callback_data*)sv.sival_ptr;
	php_luasandbox_obj * sandbox = data->sandbox;
	lua_State * L = sandbox->state;

	// It's necessary to leave the timer running while we're not actually in
	// Lua, and just ignore signals that occur outside it, because Linux timers
	// don't fire with any kind of precision. Timer delivery is routinely delayed
	// by milliseconds regardless of how short a time you ask for, and 
	// timer_gettime() just returns 1ns after the timer should have been delivered. 
	// So if a call takes 100us, there's no way to start a timer and have it be 
	// reliably delivered to within the function body, regardless of what you set 
	// it_value to.
	if (!sandbox->in_lua) {
		return;
	}

	// Set a hook which will record profiling data (but don't overwrite the timeout hook)
	if (!sandbox->timed_out) {
		int overrun;
		lua_sethook(L, luasandbox_timer_profiler_hook, 
			LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
		overrun = timer_getoverrun(sandbox->timer.profiler_timer.timer);
		sandbox->timer.profiler_signal_count += overrun + 1;
		sandbox->timer.overrun_count += overrun;

		// Reset the hook if a timeout just occurred
		if (sandbox->timed_out) {
			lua_sethook(L, luasandbox_timer_timeout_hook,
				LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
		}
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

static char * luasandbox_timer_get_cfunction_name(lua_State *L)
{
	static char buffer[1024];
	TSRMLS_FETCH();

	lua_CFunction f = lua_tocfunction(L, -1);
	if (!f) {
		return NULL;
	}
	if (f != luasandbox_call_php) {
		return NULL;
	}

	lua_getupvalue(L, -1, 1);
	zval ** callback_pp = lua_touserdata(L, -1);
	if (!callback_pp || !*callback_pp) {
		return NULL;
	}
	char * callback_name;
	if (zend_is_callable(*callback_pp, IS_CALLABLE_CHECK_SILENT, &callback_name TSRMLS_CC)) {
		snprintf(buffer, sizeof(buffer), "%s", callback_name);
		return buffer;
	} else {
		return NULL;
	}
}

static void luasandbox_timer_profiler_hook(lua_State *L, lua_Debug *ar)
{
	lua_sethook(L, luasandbox_timer_profiler_hook, 0, 0);

	int i;
	php_luasandbox_obj * sandbox = luasandbox_get_php_obj(L);
	lua_Debug debug;
	memset(&debug, 0, sizeof(debug));

	// Get and zero the signal count
	// If a signal occurs within this critical section, be careful not to lose the overrun count
	long signal_count = sandbox->timer.profiler_signal_count;
	sandbox->timer.profiler_signal_count -= signal_count;

	lua_getinfo(L, "Snlf", ar);
	const char * name = NULL;
	if (ar->what[0] == 'C') {
		name = luasandbox_timer_get_cfunction_name(L);
	}
	if (!name) {
		if (ar->namewhat[0] != '\0') {
			name = ar->name;
		} else if (ar->what[0] == 'm') {
			name = "[main chunk]";
		}
	}
	size_t prof_name_size = strlen(ar->short_src)
		+ sizeof(ar->linedefined) * 4 + sizeof("  <:>");
	if (name) {
		prof_name_size += strlen(name);
	}
	char prof_name[prof_name_size];
	if (!name) {
		if (ar->linedefined > 0) {
			snprintf(prof_name, prof_name_size, "<%s:%d>", ar->short_src, ar->linedefined);
		} else {
			strcpy(prof_name, "?");
		}
	} else {
		if (ar->what[0] == 'm') {
			snprintf(prof_name, prof_name_size, "%s <%s>", name, ar->short_src);
		} else if (ar->linedefined > 0) {
			snprintf(prof_name, prof_name_size, "%s <%s:%d>", name, ar->short_src, ar->linedefined);
		} else {
			snprintf(prof_name, prof_name_size, "%s", name);
		}
	}
	// Key length in zend_hash conventionally includes the null byte
	uint key_length = (uint)strlen(prof_name) + 1;
	ulong h = zend_inline_hash_func(prof_name, key_length);
	luasandbox_timer_set * lts = &sandbox->timer;
	HashTable * ht = lts->function_counts;
	size_t * elt;
	if (SUCCESS == zend_hash_quick_find(ht, prof_name, key_length, h, (void**)&elt)) {
		(*elt) += signal_count;
	} else {
		size_t init = signal_count;
		zend_hash_quick_add(ht, prof_name, key_length, h, (void**)&init, sizeof(size_t), NULL);
	}

	lts->total_count += signal_count;
}


void luasandbox_timer_enable_profiler(luasandbox_timer_set * lts, struct timespec * period)
{
	if (lts->profiler_running) {
		luasandbox_timer_stop_one(&lts->profiler_timer, NULL);
		lts->profiler_running = 0;
	}
	lts->profiler_period = *period;
	if (lts->function_counts) {
		zend_hash_destroy(lts->function_counts);
		lts->function_counts = NULL;
	}
	lts->total_count = 0;
	lts->overrun_count = 0;
	if (period->tv_sec || period->tv_nsec) {
		lts->function_counts = emalloc(sizeof(HashTable));
		zend_hash_init(lts->function_counts, 0, NULL, NULL, 0);

		lts->profiler_running = 1;
		luasandbox_timer_create_one(&lts->profiler_timer, lts->sandbox, LUASANDBOX_TIMER_PROFILER);
		luasandbox_timer_set_periodic(&lts->profiler_timer, &lts->profiler_period);
	}
}

void luasandbox_timer_create(luasandbox_timer_set * lts, php_luasandbox_obj * sandbox)
{
	luasandbox_timer_zero(&lts->usage);
	luasandbox_timer_zero(&lts->normal_limit);
	luasandbox_timer_zero(&lts->normal_remaining);
	luasandbox_timer_zero(&lts->emergency_limit);
	luasandbox_timer_zero(&lts->emergency_remaining);
	luasandbox_timer_zero(&lts->profiler_period);
	lts->is_running = 0;
	lts->normal_running = 0;
	lts->emergency_running = 0;
	lts->profiler_running = 0;
	lts->sandbox = sandbox;
}

void luasandbox_timer_set_limits(luasandbox_timer_set * lts,
		struct timespec * normal_timeout, 
		struct timespec * emergency_timeout)
{
	int was_running = 0;
	if (lts->is_running) {
		was_running = 1;
		luasandbox_timer_stop(lts);
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
		luasandbox_timer_create_one(&lts->normal_timer, lts->sandbox, LUASANDBOX_TIMER_NORMAL);
		luasandbox_timer_set_one_time(&lts->normal_timer, &lts->normal_remaining);
	} else {
		lts->normal_running = 0;
	}
	// Create emergency timer if requested
	if (!luasandbox_timer_is_zero(&lts->emergency_remaining)) {
		lts->emergency_running = 1;
		luasandbox_timer_create_one(&lts->emergency_timer, lts->sandbox, LUASANDBOX_TIMER_EMERGENCY);
		luasandbox_timer_set_one_time(&lts->emergency_timer, &lts->emergency_remaining);
	} else {
		lts->emergency_running = 0;
	}
}

static void luasandbox_timer_create_one(luasandbox_timer * lt, php_luasandbox_obj * sandbox, 
		int type)
{
	struct sigevent ev;

	// Make valgrind happy
	memset(&ev, 0, sizeof(ev));

	lt->cbdata.type = type;
	lt->cbdata.sandbox = sandbox;
	
	// My testing indicates that using a thread for profiler event notification
	// doesn't reduce the overrun count, but maybe it will in the future or on
	// some other system.
#ifdef LUASANDBOX_PROFILER_USE_THREAD
	if (type == LUASANDBOX_TIMER_PROFILER) {
		ev.sigev_notify = SIGEV_THREAD;
		ev.sigev_notify_function = luasandbox_timer_handle_profiler;
	} else
#endif
	{
		ev.sigev_notify = SIGEV_SIGNAL;
		ev.sigev_signo = LUASANDBOX_SIGNAL;
	}
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

static void luasandbox_timer_set_periodic(luasandbox_timer * lt, struct timespec * period)
{
	struct itimerspec its;
	its.it_interval = *period;
	its.it_value = *period;
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
	if (remaining) {
		remaining->tv_sec = its.it_value.tv_sec;
		remaining->tv_nsec = its.it_value.tv_nsec;
	}
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

void luasandbox_timer_destroy(luasandbox_timer_set * lts)
{
	luasandbox_timer_stop(lts);
	if (lts->profiler_running) {
		luasandbox_timer_stop_one(&lts->profiler_timer, NULL);
		lts->profiler_running = 0;
	}		
	if (lts->function_counts) {
		zend_hash_destroy(lts->function_counts);
		lts->function_counts = NULL;
	}
}

#endif
