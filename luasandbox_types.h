#ifndef LUASANDBOX_TYPES_H
#define LUASANDBOX_TYPES_H

#include "php.h"

#ifdef CLOCK_REALTIME

struct _php_luasandbox_obj;

typedef struct {
	int type;
	struct _php_luasandbox_obj * sandbox;
} luasandbox_timer_callback_data;

typedef struct {
	timer_t timer;
	luasandbox_timer_callback_data cbdata;
} luasandbox_timer;

typedef struct {
	luasandbox_timer normal_timer;
	luasandbox_timer emergency_timer;
	luasandbox_timer profiler_timer;
	struct timespec normal_limit, normal_remaining;
	struct timespec emergency_limit, emergency_remaining;
	struct timespec usage_start, usage;
	struct timespec profiler_period;
	struct _php_luasandbox_obj * sandbox;
	int is_running;
	int normal_running;
	int emergency_running;
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
typedef struct {} luasandbox_timer_set;

#endif /*CLOCK_REALTIME*/

ZEND_BEGIN_MODULE_GLOBALS(luasandbox)
	int signal_handler_installed;
	struct sigaction old_handler;
	HashTable * allowed_globals;
ZEND_END_MODULE_GLOBALS(luasandbox)

typedef struct {
	lua_Alloc old_alloc;
	void * old_alloc_ud;
	size_t memory_limit;
	size_t memory_usage;
	size_t peak_memory_usage;
} php_luasandbox_alloc;

struct _php_luasandbox_obj {
	zend_object std;
	lua_State * state;
	php_luasandbox_alloc alloc;
	int in_php;
	int in_lua;
	zval * current_zval; /* The zval for the LuaSandbox which is currently executing Lua code */
	volatile int timed_out;
	volatile int emergency_timed_out;
	int is_cpu_limited;
	luasandbox_timer_set timer;
	int function_index;
	unsigned int random_seed;
};
typedef struct _php_luasandbox_obj php_luasandbox_obj;

struct _php_luasandboxfunction_obj {
	zend_object std;
	zval * sandbox;
	int index;
};
typedef struct _php_luasandboxfunction_obj php_luasandboxfunction_obj;

#endif /*LUASANDBOX_TYPES_H*/

