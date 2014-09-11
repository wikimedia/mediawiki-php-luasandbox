#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <time.h>
#include <semaphore.h>
#include <lua.h>
#include <lauxlib.h>

#include "php.h"
#include "php_luasandbox.h"
#include "luasandbox_timer.h"

#ifndef LUASANDBOX_NO_CLOCK
#include <pthread.h>
#endif

char luasandbox_timeout_message[] = "The maximum execution time for this script was exceeded";

#ifdef LUASANDBOX_NO_CLOCK

void luasandbox_timer_create(luasandbox_timer_set * lts,
		php_luasandbox_obj * sandbox) {
	lts->is_paused = 0;
}
void luasandbox_timer_set_limit(luasandbox_timer_set * lts,
		struct timespec * timeout) {}
int luasandbox_timer_enable_profiler(luasandbox_timer_set * lts, struct timespec * period) {
	return 0;
}
int luasandbox_timer_start(luasandbox_timer_set * lts) {}
void luasandbox_timer_stop(luasandbox_timer_set * lts) {}
void luasandbox_timer_destroy(luasandbox_timer_set * lts) {}

void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts) {
	ts->tv_sec = ts->tv_nsec = 0;
}

void luasandbox_timer_pause(luasandbox_timer_set * lts) {
	lts->is_paused = 1;
}
void luasandbox_timer_unpause(luasandbox_timer_set * lts) {
	lts->is_paused = 0;
}
int luasandbox_timer_is_paused(luasandbox_timer_set * lts) {
	return lts->is_paused;
}

void luasandbox_timer_timeout_error(lua_State *L) {}
int luasandbox_timer_is_expired(luasandbox_timer_set * lts) {
	return 0;
}


#else

ZEND_EXTERN_MODULE_GLOBALS(luasandbox);

enum {
	LUASANDBOX_TIMER_LIMITER,
	LUASANDBOX_TIMER_PROFILER
};

static void luasandbox_timer_handle_event(union sigval sv);
static void luasandbox_timer_handle_profiler(luasandbox_timer * lt);
static void luasandbox_timer_handle_limiter(luasandbox_timer * lt);
static luasandbox_timer * luasandbox_timer_create_one(
		php_luasandbox_obj * sandbox, int type);
static luasandbox_timer * luasandbox_timer_alloc();
static void luasandbox_timer_free(luasandbox_timer *lt);
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

// Note this function is not async-signal safe. If you need to call this from a
// signal handler, you'll need to refactor the "Set a hook" part into a
// separate function and call that from the signal handler instead.
static void luasandbox_timer_handle_event(union sigval sv)
{
	luasandbox_timer * lt = (luasandbox_timer*)sv.sival_ptr;

	while (1) {
		if (!lt->sandbox) { // lt is invalid
			return;
		}

		if (!sem_wait(&lt->semaphore)) { // Got the semaphore!
			break;
		}

		if (errno != EINTR) { // Unexpected error, abort
			return;
		}
	}

	if (lt->type == LUASANDBOX_TIMER_PROFILER) {
		luasandbox_timer_handle_profiler(lt);
	} else {
		luasandbox_timer_handle_limiter(lt);
	}
	sem_post(&lt->semaphore);
}

static void luasandbox_timer_handle_profiler(luasandbox_timer * lt)
{
	// It's necessary to leave the timer running while we're not actually in
	// Lua, and just ignore signals that occur outside it, because Linux timers
	// don't fire with any kind of precision. Timer delivery is routinely delayed
	// by milliseconds regardless of how short a time you ask for, and 
	// timer_gettime() just returns 1ns after the timer should have been delivered. 
	// So if a call takes 100us, there's no way to start a timer and have it be 
	// reliably delivered to within the function body, regardless of what you set 
	// it_value to.
	php_luasandbox_obj * sandbox = lt->sandbox;
	if (!sandbox || !sandbox->in_lua) {
		return;
	}

	// Set a hook which will record profiling data (but don't overwrite the timeout hook)
	if (!sandbox->timed_out) {
		int overrun;
		lua_State * L = sandbox->state;
		lua_sethook(L, luasandbox_timer_profiler_hook, 
			LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
		overrun = timer_getoverrun(sandbox->timer.profiler_timer->timer);
		sandbox->timer.profiler_signal_count += overrun + 1;
		sandbox->timer.overrun_count += overrun;

		// Reset the hook if a timeout just occurred
		if (sandbox->timed_out) {
			lua_sethook(L, luasandbox_timer_timeout_hook,
				LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
		}
	}
}

static void luasandbox_timer_handle_limiter(luasandbox_timer * lt)
{
	lua_State * L = lt->sandbox->state;

	luasandbox_timer_set * lts = &lt->sandbox->timer;
	if (luasandbox_timer_is_paused(lts)) {
		// The timer is paused. luasandbox_timer_unpause will reschedule when unpaused.
		// Note that we need to use lt->clock_id here since CLOCK_THREAD_CPUTIME_ID
		// would get the time usage of the timer thread rather than the Lua thread.
		clock_gettime(lt->clock_id, &lts->limiter_expired_at);
	} else if (!luasandbox_timer_is_zero(&lts->pause_delta)) {
		// The timer is not paused, but we have a pause delta. Reschedule.
		luasandbox_timer_subtract(&lts->usage, &lts->pause_delta);
		lts->limiter_remaining = lts->pause_delta;
		luasandbox_timer_zero(&lts->pause_delta);
		luasandbox_timer_set_one_time(lts->limiter_timer, &lts->limiter_remaining);
	} else {
		// Set a hook which will terminate the script execution in a graceful way
		lt->sandbox->timed_out = 1;
		lua_sethook(L, luasandbox_timer_timeout_hook,
			LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE, 1);
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
	zval ** callback_pp = (zval**)lua_touserdata(L, -1);
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

int luasandbox_timer_enable_profiler(luasandbox_timer_set * lts, struct timespec * period)
{
	if (lts->profiler_running) {
		luasandbox_timer_stop_one(lts->profiler_timer, NULL);
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
		ALLOC_HASHTABLE(lts->function_counts);
		zend_hash_init(lts->function_counts, 0, NULL, NULL, 0);
		luasandbox_timer * timer = luasandbox_timer_create_one(
			lts->sandbox, LUASANDBOX_TIMER_PROFILER);
		if (!timer) {
			return 0;
		}
		lts->profiler_running = 1;
		lts->profiler_timer = timer;
		luasandbox_timer_set_periodic(timer, &lts->profiler_period);
	}
	return 1;
}

void luasandbox_timer_create(luasandbox_timer_set * lts, php_luasandbox_obj * sandbox)
{
	luasandbox_timer_zero(&lts->usage);
	luasandbox_timer_zero(&lts->limiter_limit);
	luasandbox_timer_zero(&lts->limiter_remaining);
	luasandbox_timer_zero(&lts->pause_start);
	luasandbox_timer_zero(&lts->pause_delta);
	luasandbox_timer_zero(&lts->limiter_expired_at);
	luasandbox_timer_zero(&lts->profiler_period);
	lts->is_running = 0;
	lts->limiter_running = 0;
	lts->profiler_running = 0;
	lts->sandbox = sandbox;
}

void luasandbox_timer_set_limit(luasandbox_timer_set * lts,
		struct timespec * timeout)
{
	int was_running = 0;
	int was_paused = luasandbox_timer_is_paused(lts);
	if (lts->is_running) {
		was_running = 1;
		luasandbox_timer_stop(lts);
	}
	lts->limiter_remaining = lts->limiter_limit = *timeout;
	luasandbox_timer_zero(&lts->limiter_expired_at);

	if (was_running) {
		luasandbox_timer_start(lts);
	}
	if (was_paused) {
		luasandbox_timer_pause(lts);
	}
}

int luasandbox_timer_start(luasandbox_timer_set * lts)
{
	if (lts->is_running) {
		// Already running
		return 1;
	}
	lts->is_running = 1;
	// Initialise usage timer
	clock_gettime(LUASANDBOX_CLOCK_ID, &lts->usage_start);

	// Create limiter timer if requested
	if (!luasandbox_timer_is_zero(&lts->limiter_remaining)) {
		luasandbox_timer * timer = luasandbox_timer_create_one(
			lts->sandbox, LUASANDBOX_TIMER_LIMITER);
		if (!timer) {
			lts->limiter_running = 0;
			return 0;
		}
		lts->limiter_timer = timer;
		lts->limiter_running = 1;
		luasandbox_timer_set_one_time(timer, &lts->limiter_remaining);
	} else {
		lts->limiter_running = 0;
	}
	return 1;
}

static luasandbox_timer * luasandbox_timer_create_one(
		php_luasandbox_obj * sandbox, int type)
{
	struct sigevent ev;
	luasandbox_timer * lt = luasandbox_timer_alloc();
	if (!lt) {
		return NULL;
	}

	// Make valgrind happy
	memset(&ev, 0, sizeof(ev));

	if (sem_init(&lt->semaphore, 0, 1) != 0) {
		TSRMLS_FETCH();
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"Unable to create semaphore: %s", strerror(errno));
		luasandbox_timer_free(lt);
		return NULL;
	}
	ev.sigev_notify = SIGEV_THREAD;
	ev.sigev_notify_function = luasandbox_timer_handle_event;
	lt->type = type;
	lt->sandbox = sandbox;
	ev.sigev_value.sival_ptr = (void*)lt;

	if (pthread_getcpuclockid(pthread_self(), &lt->clock_id) != 0) {
		TSRMLS_FETCH();
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"Unable to get thread clock ID: %s", strerror(errno));
		luasandbox_timer_free(lt);
		return NULL;
	}

	if (timer_create(lt->clock_id, &ev, &lt->timer) != 0) {
		TSRMLS_FETCH();
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"Unable to create timer: %s", strerror(errno));
		luasandbox_timer_free(lt);
		return NULL;
	}
	return lt;
}


/**
 * Set an interval timer.
 * Warning: This function is called from a timer event handler, and so cannot
 * call into the HHVM runtime.
 */
static void luasandbox_timer_set_one_time(luasandbox_timer * lt, struct timespec * ts)
{
	struct itimerspec its;
	luasandbox_timer_zero(&its.it_interval);
	its.it_value = *ts;
	if (luasandbox_timer_is_zero(&its.it_value)) {
		// Sanity check: make sure there is at least 1 nanosecond on the timer.
		its.it_value.tv_nsec = 1;
	}
	timer_settime(lt->timer, 0, &its, NULL);
}

/**
 * Set a periodic timer.
 */
static void luasandbox_timer_set_periodic(luasandbox_timer * lt, struct timespec * period)
{
	struct itimerspec its;
	its.it_interval = *period;
	its.it_value = *period;
	timer_settime(lt->timer, 0, &its, NULL);
}

void luasandbox_timer_stop(luasandbox_timer_set * lts)
{
	struct timespec usage, delta;

	if (lts->is_running) {
		lts->is_running = 0;
	} else {
		return;
	}

	// Make sure timers aren't paused, and extract the delta
	luasandbox_timer_unpause(lts);
	delta = lts->pause_delta;
	luasandbox_timer_zero(&lts->pause_delta);

	// Stop the limiter and save the time remaining
	if (lts->limiter_running) {
		luasandbox_timer_stop_one(lts->limiter_timer, &lts->limiter_remaining);
		lts->limiter_running = 0;
		luasandbox_timer_add(&lts->limiter_remaining, &delta);
	}

	// Update the usage
	luasandbox_update_usage(lts);
	clock_gettime(LUASANDBOX_CLOCK_ID, &usage);
	luasandbox_timer_subtract(&usage, &lts->usage_start);
	luasandbox_timer_add(&lts->usage, &usage);
	luasandbox_timer_subtract(&lts->usage, &delta);
}

static void luasandbox_timer_stop_one(luasandbox_timer * lt, struct timespec * remaining)
{
	static struct timespec zero = {0, 0};
	struct itimerspec its;
	timer_gettime(lt->timer, &its);
	if (remaining) {
		*remaining = its.it_value;
	}

	its.it_value = zero;
	its.it_interval = zero;
	timer_settime(lt->timer, 0, &its, NULL);

	// Invalidate the callback structure, delete the timer
	lt->sandbox = NULL;
	// If the timer event handler is running, wait for it to finish
	// before returning to the caller, otherwise the timer event handler
	// may find itself with a dangling pointer in its local scope.
	while (sem_wait(&lt->semaphore) && errno == EINTR);
	sem_destroy(&lt->semaphore);
	timer_delete(lt->timer);
	luasandbox_timer_free(lt);
}

static luasandbox_timer * luasandbox_timer_alloc()
{
	int start, cur;
	luasandbox_timer *timers;
	TSRMLS_FETCH();
	start = cur = LUASANDBOX_G(timer_idx) % LUASANDBOX_MAX_TIMERS;
	timers = LUASANDBOX_G(timers);
	while (1) {
		if (!timers[cur].sandbox) {
			break;
		}
		cur = (cur + 1) % LUASANDBOX_MAX_TIMERS;
		if (cur == start) {
			TSRMLS_FETCH();
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"Unable to allocate timer: %s", strerror(errno));
			return NULL;
		}
	}
	LUASANDBOX_G(timer_idx) = cur;
	return &timers[cur];
}

static void luasandbox_timer_free(luasandbox_timer *lt)
{
	lt->sandbox = NULL;
}

void luasandbox_timer_get_usage(luasandbox_timer_set * lts, struct timespec * ts)
{
	struct timespec delta;

	if (lts->is_running) {
		luasandbox_update_usage(lts);
	}
	*ts = lts->usage;
	// Subtract the pause delta from the usage
	luasandbox_timer_subtract(ts, &lts->pause_delta);
	// If currently paused, subtract the time-since-pause too
	if (!luasandbox_timer_is_zero(&lts->pause_start)) {
		clock_gettime(LUASANDBOX_CLOCK_ID, &delta);
		luasandbox_timer_subtract(&delta, &lts->pause_start);
		luasandbox_timer_subtract(ts, &delta);
	}
}

void luasandbox_timer_pause(luasandbox_timer_set * lts) {
	if (luasandbox_timer_is_zero(&lts->pause_start)) {
		clock_gettime(LUASANDBOX_CLOCK_ID, &lts->pause_start);
	}
}

void luasandbox_timer_unpause(luasandbox_timer_set * lts) {
	struct timespec delta;

	if (!luasandbox_timer_is_zero(&lts->pause_start)) {
		clock_gettime(LUASANDBOX_CLOCK_ID, &delta);
		luasandbox_timer_subtract(&delta, &lts->pause_start);

		if (luasandbox_timer_is_zero(&lts->limiter_expired_at)) {
			// Easy case, timer didn't expire while paused. Throw the whole delta
			// into pause_delta for later timer and usage adjustment.
			luasandbox_timer_add(&lts->pause_delta, &delta);
			luasandbox_timer_zero(&lts->pause_start);
		} else {
			// If the limit expired, we need to fold the whole
			// accumulated delta into usage immediately, and then restart the
			// timer with the portion before the expiry.

			// adjust usage
			luasandbox_timer_subtract(&lts->usage, &delta);
			luasandbox_timer_subtract(&lts->usage, &lts->pause_delta);

			// calculate timer delta
			delta = lts->limiter_expired_at;
			luasandbox_timer_subtract(&delta, &lts->pause_start);
			luasandbox_timer_add(&delta, &lts->pause_delta);

			// Zero out pause vars and expired timestamp (since we handled it)
			luasandbox_timer_zero(&lts->pause_start);
			luasandbox_timer_zero(&lts->pause_delta);
			luasandbox_timer_zero(&lts->limiter_expired_at);

			// Restart timer
			lts->limiter_remaining = delta;
			luasandbox_timer_set_one_time(lts->limiter_timer, &lts->limiter_remaining);
		}
	}
}

int luasandbox_timer_is_paused(luasandbox_timer_set * lts) {
	return !luasandbox_timer_is_zero(&lts->pause_start);
}

int luasandbox_timer_is_expired(luasandbox_timer_set * lts)
{
	if (!luasandbox_timer_is_zero(&lts->limiter_limit)) {
		if (luasandbox_timer_is_zero(&lts->limiter_remaining)) {
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
		luasandbox_timer_stop_one(lts->profiler_timer, NULL);
		lts->profiler_running = 0;
	}
	if (lts->function_counts) {
		zend_hash_destroy(lts->function_counts);
		lts->function_counts = NULL;
	}
}

#endif
