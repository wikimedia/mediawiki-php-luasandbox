
#ifndef PHP_LUASANDBOX_H
#define PHP_LUASANDBOX_H

#include <lua.h>
#include <signal.h>

/* alloc.c */

typedef struct {
	lua_Alloc old_alloc;
	void * old_alloc_ud;
	size_t memory_limit;
	size_t memory_usage;
} php_luasandbox_alloc;

struct _php_luasandbox_obj;

lua_State * luasandbox_alloc_new_state(php_luasandbox_alloc * alloc, struct _php_luasandbox_obj * sandbox);
void luasandbox_alloc_delete_state(php_luasandbox_alloc * alloc, lua_State * L);

/* luasandbox.c */

extern zend_module_entry luasandbox_module_entry;

#define phpext_luasandbox_ptr &luasandbox_module_entry

#ifdef PHP_WIN32
#	define PHP_LUASANDBOX_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_LUASANDBOX_API __attribute__ ((visibility("default")))
#else
#	define PHP_LUASANDBOX_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(luasandbox);
PHP_MSHUTDOWN_FUNCTION(luasandbox);
PHP_RSHUTDOWN_FUNCTION(luasandbox);
PHP_MINFO_FUNCTION(luasandbox);

PHP_METHOD(LuaSandbox, loadString);
PHP_METHOD(LuaSandbox, loadBinary);
PHP_METHOD(LuaSandbox, setMemoryLimit);
PHP_METHOD(LuaSandbox, getMemoryUsage);
PHP_METHOD(LuaSandbox, setCPULimit);
PHP_METHOD(LuaSandbox, callFunction);
PHP_METHOD(LuaSandbox, registerLibrary);

PHP_METHOD(LuaSandboxFunction, __construct);
PHP_METHOD(LuaSandboxFunction, call);
PHP_METHOD(LuaSandboxFunction, dump);

ZEND_BEGIN_MODULE_GLOBALS(luasandbox)
	int signal_handler_installed;
	struct sigaction old_handler;
	HashTable * allowed_globals;
ZEND_END_MODULE_GLOBALS(luasandbox)

#ifdef ZTS
#define LUASANDBOX_G(v) TSRMG(luasandbox_globals_id, zend_luasandbox_globals *, v)
#else
#define LUASANDBOX_G(v) (luasandbox_globals.v)
#endif

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
	struct timespec cpu_normal_limit;
	struct timespec cpu_emergency_limit;
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


php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L);
void luasandbox_timer_timeout_error(lua_State *L);

/** {{{ luasandbox_enter_php
 *
 * This function must be called each time a C function is entered from Lua
 * and the PHP state needs to be accessed in any way. Before exiting the 
 * function, luasandbox_leave_php() must be called.
 *
 * This sets a flag which indicates to the timeout signal handler that it is 
 * unsafe to call longjmp() to return control to PHP. If the flag is not 
 * correctly set, memory may be corrupted and security compromised.
 */
static inline void luasandbox_enter_php(lua_State * L, php_luasandbox_obj * intern)
{
	intern->in_php ++;
	if (intern->timed_out) {
		intern->in_php --;
		luasandbox_timer_timeout_error(L);
	}
}
/* }}} */

/** {{{ luasandbox_leave_php
 *
 * This function must be called after luasandbox_enter_php, before the callback 
 * from Lua returns.
 */
static inline void luasandbox_leave_php(lua_State * L, php_luasandbox_obj * intern)
{
	intern->in_php --;
}
/* }}} */

/* library.c */

void luasandbox_lib_register(lua_State * L TSRMLS_DC);
void luasandbox_lib_shutdown(TSRMLS_D);

/* data_conversion.c */

void luasandbox_data_conversion_init(lua_State * L);

int luasandbox_push_zval(lua_State * L, zval * z);
void luasandbox_push_zval_userdata(lua_State * L, zval * z);
void luasandbox_lua_to_zval(zval * z, lua_State * L, int index, 
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC);
void luasandbox_wrap_fatal(lua_State * L);
int luasandbox_is_fatal(lua_State * L, int index);
int luasandbox_is_trace_error(lua_State * L, int index);
const char * luasandbox_error_to_string(lua_State * L, int index);
int luasandbox_attach_trace(lua_State * L);
void luasandbox_push_structured_trace(lua_State * L, int level);


#endif	/* PHP_LUASANDBOX_H */

