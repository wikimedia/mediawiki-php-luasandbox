
#ifndef PHP_LUASANDBOX_H
#define PHP_LUASANDBOX_H

#include <lua.h>
#include <lualib.h>
#include <signal.h>

#include "luasandbox_types.h"
#include "luasandbox_timer.h"

/* alloc.c */


lua_State * luasandbox_alloc_new_state(php_luasandbox_alloc * alloc, php_luasandbox_obj * sandbox);
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

int luasandbox_call_php(lua_State * L);
int luasandbox_call_lua(php_luasandbox_obj * sandbox, zval * sandbox_zval,
	int nargs, int nresults, int errfunc TSRMLS_DC);

PHP_MINIT_FUNCTION(luasandbox);
PHP_MSHUTDOWN_FUNCTION(luasandbox);
PHP_RSHUTDOWN_FUNCTION(luasandbox);
PHP_MINFO_FUNCTION(luasandbox);

PHP_METHOD(LuaSandbox, getVersionInfo);
PHP_METHOD(LuaSandbox, loadString);
PHP_METHOD(LuaSandbox, loadBinary);
PHP_METHOD(LuaSandbox, setMemoryLimit);
PHP_METHOD(LuaSandbox, getMemoryUsage);
PHP_METHOD(LuaSandbox, getPeakMemoryUsage);
PHP_METHOD(LuaSandbox, setCPULimit);
PHP_METHOD(LuaSandbox, getCPUUsage);
PHP_METHOD(LuaSandbox, pauseUsageTimer);
PHP_METHOD(LuaSandbox, unpauseUsageTimer);
PHP_METHOD(LuaSandbox, enableProfiler);
PHP_METHOD(LuaSandbox, disableProfiler);
#ifdef HHVM
PHP_METHOD(LuaSandbox, _internal_getProfilerFunctionReport);
#else
PHP_METHOD(LuaSandbox, getProfilerFunctionReport);
#endif
PHP_METHOD(LuaSandbox, callFunction);
PHP_METHOD(LuaSandbox, wrapPhpFunction);
PHP_METHOD(LuaSandbox, registerLibrary);

PHP_METHOD(LuaSandboxFunction, __construct);
PHP_METHOD(LuaSandboxFunction, call);
PHP_METHOD(LuaSandboxFunction, dump);

#ifdef ZTS
#define LUASANDBOX_G(v) TSRMG(luasandbox_globals_id, zend_luasandbox_globals *, v)
#else
#define LUASANDBOX_G(v) (luasandbox_globals.v)
#endif


php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L);

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

/**
 * {{ luasandbox_enter_php_ignore_timeouts
 *
 * Like luasandbox_enter_php except that no error is raised if a timeout has occurred
 */
static inline void luasandbox_enter_php_ignore_timeouts(lua_State * L, php_luasandbox_obj * intern)
{
	intern->in_php ++;
}

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
void luasandbox_lib_destroy_globals(TSRMLS_D);

/* luasandbox_lstrlib.c */

int luasandbox_open_string(lua_State * L);

/* data_conversion.c */

void luasandbox_data_conversion_init(lua_State * L);

int luasandbox_push_zval(lua_State * L, zval * z, HashTable *recursionGuard);
void luasandbox_push_zval_userdata(lua_State * L, zval * z);
int luasandbox_lua_to_zval(zval * z, lua_State * L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC);
void luasandbox_wrap_fatal(lua_State * L);
int luasandbox_is_fatal(lua_State * L, int index);
int luasandbox_is_trace_error(lua_State * L, int index);
const char * luasandbox_error_to_string(lua_State * L, int index);
int luasandbox_attach_trace(lua_State * L);
void luasandbox_push_structured_trace(lua_State * L, int level);

#endif	/* PHP_LUASANDBOX_H */

