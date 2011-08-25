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

#ifdef LUASANDBOX_NO_CLOCK

void luasandbox_timer_install_handler(struct sigaction * oldact) {}
void luasandbox_timer_remove_handler(struct sigaction * oldact) {}
void luasandbox_timer_start(luasandbox_timer_set * lts, 
		php_luasandbox_obj * sandbox,
		struct timespec * normal_timeout,
		struct timespec * emergency_timeout) {}
void luasandbox_timer_stop(luasandbox_timer_set * lts) {}

#else

static void luasandbox_timer_handle(int signo, siginfo_t * info, void * context);
static void luasandbox_timer_create(luasandbox_timer * lt, php_luasandbox_obj * sandbox, 
		int emergency);
static void luasandbox_timer_timeout_hook(lua_State *L, lua_Debug *ar);
static void luasandbox_timer_settime(luasandbox_timer * lt, struct timespec * ts);

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
	
	if (signo != LUASANDBOX_SIGNAL
			|| info->si_code != SI_TIMER
			|| !info->si_value.sival_ptr)
	{
		return;
	}

	data = (luasandbox_timer_callback_data*)info->si_value.sival_ptr;
	data->sandbox->timed_out = 1;
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
			luaL_error(data->sandbox->state, "emergency timeout");
		}
	} else {
		// Set a hook which will terminate the script execution in a graceful way
		lua_sethook(data->sandbox->state, luasandbox_timer_timeout_hook,
			LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
	}
}

static void luasandbox_timer_timeout_hook(lua_State *L, lua_Debug *ar)
{
	// Avoid infinite recursion
	lua_sethook(L, luasandbox_timer_timeout_hook, 0, 0);
	// Do a longjmp to report the timeout error
	luaL_error(L, luasandbox_timeout_message);
}

void luasandbox_timer_start(luasandbox_timer_set * lts, 
		php_luasandbox_obj * sandbox,
		struct timespec * normal_timeout,
		struct timespec * emergency_timeout)
{
	// Create normal timer
	luasandbox_timer_create(&lts->normal_timer, sandbox, 0);
	luasandbox_timer_settime(&lts->normal_timer, normal_timeout);

	// Create emergency timer if requested
	if (emergency_timeout->tv_sec || emergency_timeout->tv_nsec) {
		lts->use_emergency = 1;
		luasandbox_timer_create(&lts->emergency_timer, sandbox, 1);
		luasandbox_timer_settime(&lts->emergency_timer, emergency_timeout);
	} else {
		lts->use_emergency = 0;
	}
}

static void luasandbox_timer_create(luasandbox_timer * lt, php_luasandbox_obj * sandbox, 
		int emergency)
{
	struct sigevent ev;

	lt->cbdata.emergency = emergency;
	lt->cbdata.sandbox = sandbox;
	
	ev.sigev_notify = SIGEV_SIGNAL;
	ev.sigev_signo = LUASANDBOX_SIGNAL;
	ev.sigev_value.sival_ptr = (void*)&lt->cbdata;

	timer_create(LUASANDBOX_CLOCK_ID, &ev, &lt->timer);
}

static void luasandbox_timer_settime(luasandbox_timer * lt, struct timespec * ts)
{
	struct itimerspec its;
	its.it_interval.tv_sec = 0;
	its.it_interval.tv_nsec = 0;
	its.it_value = *ts;
	timer_settime(lt->timer, 0, &its, NULL);
}

void luasandbox_timer_stop(luasandbox_timer_set * lts)
{
	struct timespec zero = {0, 0};
	if (lts->use_emergency) {
		luasandbox_timer_settime(&lts->emergency_timer, &zero);
		timer_delete(lts->emergency_timer.timer);
	}

	luasandbox_timer_settime(&lts->normal_timer, &zero);
	timer_delete(lts->normal_timer.timer);
}

#endif
