#ifndef LUASANDBOX_TYPES_H
#define LUASANDBOX_TYPES_H

#include <semaphore.h>
#include "php.h"

#ifdef CLOCK_REALTIME

struct _php_luasandbox_obj;

typedef struct _luasandbox_timer {
	struct _php_luasandbox_obj * sandbox;
	timer_t timer;
	clockid_t clock_id;
	int type;
	sem_t semaphore;
	int id;
} luasandbox_timer;

typedef struct {
	luasandbox_timer *limiter_timer;
	luasandbox_timer *profiler_timer;
	struct timespec limiter_limit, limiter_remaining;
	struct timespec usage_start, usage;
	struct timespec pause_start, pause_delta;
	struct timespec limiter_expired_at;
	struct timespec profiler_period;
	struct _php_luasandbox_obj * sandbox;
	int is_running;
	int limiter_running;
	int profiler_running;

	// A HashTable storing the number of times each function was hit by the
	// profiler. The data is a size_t because that hits a special case in
	// zend_hash which avoids the need to allocate separate space for the data
	// on the heap.
	HashTable * function_counts;

	// The total number of samples recorded in function_counts
	long total_count;

	// The number of timer expirations that have occurred since the profiler hook
	// was last run
	volatile long profiler_signal_count;

	volatile long overrun_count;
} luasandbox_timer_set;

#else /*CLOCK_REALTIME*/

typedef struct {} luasandbox_timer;
typedef struct {
	struct timespec profiler_period;
	HashTable * function_counts;
	long total_count;
	int is_paused;
} luasandbox_timer_set;

#endif /*CLOCK_REALTIME*/

ZEND_BEGIN_MODULE_GLOBALS(luasandbox)
	/* Stored as a value rather than a pointer to avoid segfaults. Inspired by https://github.com/php/php-src/blob/master/ext/pcre/php_pcre.c.*/
	HashTable allowed_globals;
	long active_count;
ZEND_END_MODULE_GLOBALS(luasandbox)

typedef struct {
	lua_Alloc old_alloc;
	void * old_alloc_ud;
	size_t memory_limit;
	size_t memory_usage;
	size_t peak_memory_usage;
} php_luasandbox_alloc;

struct _php_luasandbox_obj {
	lua_State * state;
	php_luasandbox_alloc alloc;
	int in_php;
	int in_lua;
	zval current_zval; /* The zval for the LuaSandbox which is currently executing Lua code */
	volatile int timed_out;
	int is_cpu_limited;
	luasandbox_timer_set timer;
	int function_index;
	unsigned int random_seed;
	int allow_pause;
	zend_object std;
};
typedef struct _php_luasandbox_obj php_luasandbox_obj;

struct _php_luasandboxfunction_obj {
	zval sandbox;
	int index;
	zend_object std;
};
typedef struct _php_luasandboxfunction_obj php_luasandboxfunction_obj;

// Accessor macros
static inline php_luasandbox_obj *php_luasandbox_fetch_object(zend_object *obj) {
	return (php_luasandbox_obj *)((char*)(obj) - XtOffsetOf(php_luasandbox_obj, std));
}

static inline php_luasandboxfunction_obj *php_luasandboxfunction_fetch_object(zend_object *obj) {
	return (php_luasandboxfunction_obj *)((char*)(obj) - XtOffsetOf(php_luasandboxfunction_obj, std));
}

#define GET_LUASANDBOX_OBJ(z) php_luasandbox_fetch_object(Z_OBJ_P(z))
#define GET_LUASANDBOXFUNCTION_OBJ(z) php_luasandboxfunction_fetch_object(Z_OBJ_P(z))
#define LUASANDBOXFUNCTION_SANDBOX_IS_OK(pfunc) !Z_ISUNDEF((pfunc)->sandbox)
#define LUASANDBOXFUNCTION_GET_SANDBOX_ZVALPTR(pfunc) &((pfunc)->sandbox)
#define LUASANDBOX_GET_CURRENT_ZVAL_PTR(psandbox) &((psandbox)->current_zval)

#endif /*LUASANDBOX_TYPES_H*/
