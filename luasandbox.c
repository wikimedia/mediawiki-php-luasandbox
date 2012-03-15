#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "php_luasandbox.h"
#include "luasandbox_timer.h"
#include "ext/standard/php_smart_str.h"

#define CHECK_VALID_STATE(state) \
	if (!state) { \
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid LuaSandbox state"); \
		RETURN_FALSE; \
	}

static zend_object_value luasandbox_new(zend_class_entry *ce TSRMLS_DC);
static lua_State * luasandbox_newstate(php_luasandbox_obj * intern);
static void luasandbox_free_storage(void *object TSRMLS_DC);
static zend_object_value luasandboxfunction_new(zend_class_entry *ce TSRMLS_CC);
static void luasandboxfunction_free_storage(void *object TSRMLS_DC);
static void luasandboxfunction_destroy(void *object, zend_object_handle handle TSRMLS_DC);
static int luasandbox_panic(lua_State * L);
static lua_State * luasandbox_state_from_zval(zval * this_ptr TSRMLS_DC);
static void luasandbox_load_helper(int binary, INTERNAL_FUNCTION_PARAMETERS);
static int luasandbox_find_field(lua_State * L, int index, 
	char * spec, int specLength);
static void luasandbox_set_timespec(struct timespec * dest, double source);
static int luasandbox_function_init(zval * this_ptr, php_luasandboxfunction_obj ** pfunc, 
	lua_State ** pstate, php_luasandbox_obj ** psandbox TSRMLS_DC);
static void luasandbox_call_helper(lua_State * L, zval * sandbox_zval, 
	php_luasandbox_obj * sandbox,
	zval *** args, zend_uint numArgs, zval * return_value TSRMLS_DC);
static void luasandbox_handle_error(lua_State * L, int status);
static void luasandbox_handle_emergency_timeout(php_luasandbox_obj * sandbox);
static int luasandbox_call_php(lua_State * L);
static int luasandbox_dump_writer(lua_State * L, const void * p, size_t sz, void * ud);

char luasandbox_timeout_message[] = "The maximum execution time for this script was exceeded";

zend_class_entry *luasandbox_ce;
zend_class_entry *luasandboxerror_ce;
zend_class_entry *luasandboxemergencytimeout_ce;
zend_class_entry *luasandboxplaceholder_ce;
zend_class_entry *luasandboxfunction_ce;

ZEND_DECLARE_MODULE_GLOBALS(luasandbox);

/** {{{ arginfo */
ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_loadString, 0)
	ZEND_ARG_INFO(0, code)
	ZEND_ARG_INFO(0, chunkName)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_loadBinary, 0)
	ZEND_ARG_INFO(0, code)
	ZEND_ARG_INFO(0, chunkName)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setMemoryLimit, 0)
	ZEND_ARG_INFO(0, limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_getMemoryUsage, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setCPULimit, 0)
	ZEND_ARG_INFO(0, normal_limit)
	ZEND_ARG_INFO(0, emergency_limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_callFunction, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, ...)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_registerLibrary, 0)
	ZEND_ARG_INFO(0, libname)
	ZEND_ARG_INFO(0, functions)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandboxfunction___construct, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandboxfunction_call, 0)
	ZEND_ARG_INFO(0, ...)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandboxfunction_dump, 0)
ZEND_END_ARG_INFO()

/* }}} */

/** {{{ function entries */
const zend_function_entry luasandbox_functions[] = {
	{NULL, NULL, NULL}	/* Must be the last line in luasandbox_functions[] */
};

const zend_function_entry luasandbox_methods[] = {
	PHP_ME(LuaSandbox, loadString, arginfo_luasandbox_loadString, 0)
	PHP_ME(LuaSandbox, loadBinary, arginfo_luasandbox_loadBinary, 0)
	PHP_ME(LuaSandbox, setMemoryLimit, arginfo_luasandbox_setMemoryLimit, 0)
	PHP_ME(LuaSandbox, getMemoryUsage, arginfo_luasandbox_getMemoryUsage, 0)
	PHP_ME(LuaSandbox, setCPULimit, arginfo_luasandbox_setCPULimit, 0)
	PHP_ME(LuaSandbox, callFunction, arginfo_luasandbox_callFunction, 0)
	PHP_ME(LuaSandbox, registerLibrary, arginfo_luasandbox_registerLibrary, 0)
	{NULL, NULL, NULL}
};

const zend_function_entry luasandboxfunction_methods[] = {
	PHP_ME(LuaSandboxFunction, __construct, arginfo_luasandboxfunction___construct, 
		ZEND_ACC_PRIVATE | ZEND_ACC_FINAL)
	PHP_ME(LuaSandboxFunction, call, arginfo_luasandboxfunction_call, 0)
	PHP_ME(LuaSandboxFunction, dump, arginfo_luasandboxfunction_dump, 0)
	{NULL, NULL, NULL}
};

const zend_function_entry luasandbox_empty_methods[] = {
	{NULL, NULL, NULL}
};

/* }}} */

/* {{{ luasandbox_module_entry
 */
zend_module_entry luasandbox_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"luasandbox",
	luasandbox_functions,
	PHP_MINIT(luasandbox),
	PHP_MSHUTDOWN(luasandbox),
	NULL, /* RINIT */
	PHP_RSHUTDOWN(luasandbox), /* RSHUTDOWN */
	PHP_MINFO(luasandbox),
#if ZEND_MODULE_API_NO >= 20010901
	"0.1",
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_LUASANDBOX
ZEND_GET_MODULE(luasandbox)
#endif

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(luasandbox)
{
	/* If you have INI entries, uncomment these lines 
	REGISTER_INI_ENTRIES();
	*/

	zend_class_entry ce;
	INIT_CLASS_ENTRY(ce, "LuaSandbox", luasandbox_methods);
	luasandbox_ce = zend_register_internal_class(&ce TSRMLS_CC);
	luasandbox_ce->create_object = luasandbox_new;

	INIT_CLASS_ENTRY(ce, "LuaSandboxError", luasandbox_empty_methods);
	luasandboxerror_ce = zend_register_internal_class_ex(
			&ce, zend_exception_get_default(TSRMLS_C), NULL TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "LuaSandboxEmergencyTimeout", luasandbox_empty_methods);
	luasandboxemergencytimeout_ce = zend_register_internal_class_ex(
		&ce, luasandboxerror_ce, NULL TSRMLS_CC);

	zend_declare_class_constant_long(luasandboxerror_ce, 
		"RUN", sizeof("RUN"), LUA_ERRRUN);
	zend_declare_class_constant_long(luasandboxerror_ce,
		"SYNTAX", sizeof("SYNTAX"), LUA_ERRSYNTAX);
	zend_declare_class_constant_long(luasandboxerror_ce,
		"MEM", sizeof("MEM"), LUA_ERRMEM);
	zend_declare_class_constant_long(luasandboxerror_ce,
		"ERR", sizeof("ERR"), LUA_ERRERR);

	INIT_CLASS_ENTRY(ce, "LuaSandboxPlaceholder", luasandbox_empty_methods);
	luasandboxplaceholder_ce = zend_register_internal_class(&ce TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "LuaSandboxFunction", luasandboxfunction_methods);
	luasandboxfunction_ce = zend_register_internal_class(&ce TSRMLS_CC);
	luasandboxfunction_ce->create_object = luasandboxfunction_new;
	
	LUASANDBOX_G(allowed_globals) = NULL;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(luasandbox)
{
	luasandbox_lib_shutdown(TSRMLS_C);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(luasandbox)
{
	if (LUASANDBOX_G(signal_handler_installed)) {
		luasandbox_timer_remove_handler(&LUASANDBOX_G(old_handler));
	}
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(luasandbox)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "luasandbox support", "enabled");
	php_info_print_table_end();
}
/* }}} */

/** {{{ luasandbox_new
 *
 * "new" handler for the LuaSandbox class
 */
static zend_object_value luasandbox_new(zend_class_entry *ce TSRMLS_DC)
{
	php_luasandbox_obj * intern;
	zend_object_value retval;

	// Create the internal object
	intern = emalloc(sizeof(php_luasandbox_obj));
	memset(intern, 0, sizeof(php_luasandbox_obj));
	zend_object_std_init(&intern->std, ce TSRMLS_CC);
	intern->alloc.memory_limit = (size_t)-1;

	// Initialise the Lua state
	intern->state = luasandbox_newstate(intern);

	// Put the object into the store
	retval.handle = zend_objects_store_put(
		intern,
		(zend_objects_store_dtor_t)zend_objects_destroy_object, 
		(zend_objects_free_object_storage_t)luasandbox_free_storage, 
		NULL TSRMLS_CC);
	retval.handlers = zend_get_std_object_handlers();
	return retval;
}
/* }}} */

/** {{{ luasandbox_newstate 
 *
 * Create a new lua_State which is suitable for running sandboxed scripts in.
 * Initialise libraries and any necessary registry entries.
 */
static lua_State * luasandbox_newstate(php_luasandbox_obj * intern) 
{
	lua_State * L = luasandbox_alloc_new_state(&intern->alloc, intern);

	if (L == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR,
			"Attempt to allocate a new Lua state failed");
	}

	lua_atpanic(L, luasandbox_panic);
	
	// Register the standard library
	luasandbox_lib_register(L TSRMLS_CC);

	// Set up the data conversion module
	luasandbox_data_conversion_init(L);

	// Create a table for storing chunks
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
	
	// Register a pointer to the PHP object so that C functions can find it
	lua_pushlightuserdata(L, (void*)intern);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");

	return L;
}
/* }}} */

/** {{{ luasandbox_free_storage
 *
 * "Free storage" handler for LuaSandbox objects.
 */
static void luasandbox_free_storage(void *object TSRMLS_DC)
{
	php_luasandbox_obj * intern = (php_luasandbox_obj*)object;

	if (intern->state) {
		luasandbox_alloc_delete_state(&intern->alloc, intern->state);
		intern->state = NULL;
		
		intern->state = NULL;
	}
	zend_object_std_dtor(&intern->std);
	efree(object);
}
/* }}} */

/** {{{ luasandboxfunction_new
 *
 * "new" handler for the LuaSandboxFunction class.
 */
static zend_object_value luasandboxfunction_new(zend_class_entry *ce TSRMLS_CC)
{
	php_luasandboxfunction_obj * intern;
	zend_object_value retval;
	
	// Create the internal object
	intern = emalloc(sizeof(php_luasandboxfunction_obj));
	memset(intern, 0, sizeof(php_luasandboxfunction_obj));
	zend_object_std_init(&intern->std, ce TSRMLS_CC);

	// Put the object into the store
	retval.handle = zend_objects_store_put(
		intern,
		(zend_objects_store_dtor_t)luasandboxfunction_destroy,
		(zend_objects_free_object_storage_t)luasandboxfunction_free_storage,
		NULL TSRMLS_CC);
	retval.handlers = zend_get_std_object_handlers();
	return retval;
}
/* }}} */

/** {{{ luasandboxfunction_destroy 
 *
 * Destructor for the LuaSandboxFunction class. Deletes the chunk from the
 * registry and decrements the reference counter for the parent LuaSandbox
 * object.
 */
static void luasandboxfunction_destroy(void *object, zend_object_handle handle TSRMLS_DC)
{
	php_luasandboxfunction_obj * func = (php_luasandboxfunction_obj*)object;
	if (func->sandbox) {
		php_luasandbox_obj * sandbox = (php_luasandbox_obj*)
			zend_object_store_get_object(func->sandbox TSRMLS_CC);
		if (sandbox && sandbox->state) {
			lua_State * L = sandbox->state;

			// Delete the chunk
			if (func->index) {
				lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
				lua_pushnil(L);
				lua_rawseti(L, -2, func->index);
				lua_pop(L, 1);
			}
		}

		// Delete the parent reference
		zval_ptr_dtor(&func->sandbox);
	}
}
/* }}} */

/** {{{ luasandboxfunction_free_storage
 *
 * "Free storage" handler for LuaSandboxFunction objects.
 */
static void luasandboxfunction_free_storage(void *object TSRMLS_DC)
{
	php_luasandboxfunction_obj * func = (php_luasandboxfunction_obj*)object;
	zend_object_std_dtor(&func->std);
	efree(object);
}
/* }}} */

/** {{{ luasandbox_panic
 *
 * The Lua panic function. It is necessary to raise an E_ERROR, and thus do a
 * longjmp(), since if this function returns, Lua will call exit(), breaking 
 * the Apache child.
 *
 * Generally, a panic will happen if the Lua API is used incorrectly in an
 * unprotected environment. Currently this means C code which is called from
 * PHP, not directly or indirectly from lua_pcall(). Sandboxed Lua code is run
 * under lua_pcall() so can't cause a panic.
 *
 * Note that sandboxed Lua code may be executed in an unprotected environment
 * if C code accesses a variable with a metamethod defined by the sandboxed 
 * code. For this reason, the "raw" functions such as lua_rawget() should be 
 * used where this is a possibility.
 */
static int luasandbox_panic(lua_State * L)
{
	php_error_docref(NULL TSRMLS_CC, E_ERROR,
		"PANIC: unprotected error in call to Lua API (%s)",
		lua_tostring(L, -1));
	return 0;
}
/* }}} */

/** {{{ luasandbox_state_from_zval
 *
 * Get a lua state from a zval* containing a LuaSandbox object. If the zval* 
 * contains something else, bad things will happen.
 */
static lua_State * luasandbox_state_from_zval(zval * this_ptr TSRMLS_DC)
{
	php_luasandbox_obj * intern = (php_luasandbox_obj*) 
		zend_object_store_get_object(this_ptr TSRMLS_CC);
	return intern->state;
}
/* }}} */

/** {{{ luasandbox_load_helper
 *
 * Common code for LuaSandbox::loadString() and LuaSandbox::loadBinary(). The
 * "binary" parameter will be 1 for loadBinary() and 0 for loadString().
 */
static void luasandbox_load_helper(int binary, INTERNAL_FUNCTION_PARAMETERS)
{
	char *code, *chunkName = NULL;
	int codeLength, chunkNameLength;
	int status;
	lua_State * L;
	size_t index;
	php_luasandboxfunction_obj * func_obj;
	int have_mark;
	php_luasandbox_obj * sandbox;
	
	sandbox = (php_luasandbox_obj*) 
		zend_object_store_get_object(this_ptr TSRMLS_CC);
	L = sandbox->state;
	CHECK_VALID_STATE(L);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s", 
				&code, &codeLength, &chunkName, &chunkNameLength) == FAILURE) {
		RETURN_FALSE;
	}

	if (chunkName == NULL) {
		chunkName = "";
	} else {
		// Check chunkName for nulls
		if (strlen(chunkName) != chunkNameLength) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, 
				"chunk name may not contain null characters");
			RETURN_FALSE;
		}
	}

	// Check to see if the code is binary (with precompiled data mark) if this 
	// was called as loadBinary(), and plain code (without mark) if this was
	// called as loadString()
	have_mark = (php_memnstr(code, LUA_SIGNATURE, 
		sizeof(LUA_SIGNATURE) - 1, code + codeLength) != NULL);
	if (binary && !have_mark) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"the string does not appear to be a valid binary chunk");
		RETURN_FALSE;
	} else if (!binary && have_mark) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"cannot load code with a Lua binary chunk marker escape sequence in it");
		RETURN_FALSE;
	}

	// Parse the string into a function on the stack
	status = luaL_loadbuffer(L, code, codeLength, chunkName);
	if (status != 0) {
		luasandbox_handle_error(L, status);
		return;
	}

	// Make a zval out of it, and return false on error
	luasandbox_lua_to_zval(return_value, L, lua_gettop(L), this_ptr, NULL TSRMLS_CC);
	if (Z_TYPE_P(return_value) == IS_NULL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"too many chunks loaded already");
		RETVAL_FALSE;
	}

	// Balance the stack
	lua_pop(L, 1);
}
/* }}} */

/** {{{ proto LuaSandboxFunction LuaSandbox::loadString(string code, string chunkName) 
 *
 * Load a string into the LuaSandbox object. Returns a LuaSandboxFunction object 
 * which can be called or dumped.
 *
 * Note that global functions and variables defined in the chunk will not be 
 * present in the Lua state until the chunk is executed. Function definitions
 * in Lua are a kind of executable statement.
 */
PHP_METHOD(LuaSandbox, loadString)
{
	luasandbox_load_helper(0, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* }}} */

/** {{{ proto LuaSandboxFunction LuaSandbox::loadBinary(string bin, string chunkName)
 * Load a string containing a precompiled binary chunk from 
 * LuaSandboxFunction::dump() or the Lua compiler luac. Returns a 
 * LuaSandboxFunction object.
 */
PHP_METHOD(LuaSandbox, loadBinary)
{
	luasandbox_load_helper(1, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/** {{{ luasandbox_handle_emergency_timeout
 *
 * Handle the situation where the emergency_timeout flag is set. Throws an 
 * appropriate exception and destroys the state.
 */
static void luasandbox_handle_emergency_timeout(php_luasandbox_obj * sandbox)
{
	lua_close(sandbox->state);
	sandbox->state = NULL;
	sandbox->emergency_timed_out = 0;
	zend_throw_exception(luasandboxemergencytimeout_ce, 
		"The maximum execution time was exceeded "
		"and the current Lua statement failed to return, leading to "
		"destruction of the Lua state", LUA_ERRRUN);
}
/* }}} */

/** {{{ luasandbox_handle_error
 *
 * Handles the error return situation from lua_pcall() and lua_load(), where a 
 * status is returned and an error message pushed to the stack. Throws a suitable
 * exception.
 */
static void luasandbox_handle_error(lua_State * L, int status)
{
	const char * errorMsg = lua_tostring(L, -1);
	lua_pop(L, 1);
	if (!EG(exception)) {
		zend_throw_exception(luasandboxerror_ce, (char*)errorMsg, status);
	}
}
/* }}} */

/** {{{ proto void LuaSandbox::setMemoryLimit(int limit)
 *
 * Set the memory limit for the Lua state. If the memory limit is exceeded,
 * the allocator will return NULL. When running protected code, this will
 * result in a LuaSandboxError exception being thrown. If this occurs in 
 * unprotected code, say due to loading too many functions with loadString(),
 * a panic will be triggered, leading to a PHP fatal error.
 */
PHP_METHOD(LuaSandbox, setMemoryLimit)
{
	long limit;
	php_luasandbox_obj * intern = (php_luasandbox_obj*) 
		zend_object_store_get_object(getThis() TSRMLS_CC);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", 
				&limit) == FAILURE) 
	{
		RETURN_FALSE;
	}

	intern->alloc.memory_limit = limit;
}
/* }}} */


/** {{{ proto void LuaSandbox::setCPULimit(mixed normal_limit, mixed emergency_limit = false)
 *
 * Set the limit of CPU time (user+system) for LuaSandboxFunction::call()
 * calls. There are two time limits, which are both specified in seconds. Set
 * a time limit to false to disable it.
 *
 * When the "normal" time limit expires, a flag will be set which causes 
 * a LuaSandboxError exception to be thrown when the currently-running Lua 
 * statement finishes.
 *
 * When the "emergency" time limit expires, execution is terminated immediately.
 * If PHP code was running, this results in the current PHP request being in an
 * undefined state, so a PHP fatal error is raised. If PHP code was not
 * running, the Lua state is destroyed and then recreated with an empty state. 
 * Any LuaSandboxFunction objects which referred to the old state will stop 
 * working. A LuaSandboxEmergencyTimeout exception is thrown.
 */
PHP_METHOD(LuaSandbox, setCPULimit)
{
	zval **zpp_normal = NULL, **zpp_emergency = NULL;

	php_luasandbox_obj * sandbox = (php_luasandbox_obj*) 
		zend_object_store_get_object(getThis() TSRMLS_CC);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Z|Z",
		&zpp_normal, &zpp_emergency) == FAILURE)
	{
		RETURN_FALSE;
	}

	if (!zpp_normal
		|| (Z_TYPE_PP(zpp_normal) == IS_BOOL && Z_BVAL_PP(zpp_normal) == 0))
	{
		// No limit
		sandbox->is_cpu_limited = 0;
	} else {
		convert_scalar_to_number_ex(zpp_normal);
		if (Z_TYPE_PP(zpp_normal) == IS_LONG) {
			convert_to_double_ex(zpp_normal);
		}
		if (Z_TYPE_PP(zpp_normal) == IS_DOUBLE) {
			luasandbox_set_timespec(&sandbox->cpu_normal_limit, Z_DVAL_PP(zpp_normal));
			sandbox->is_cpu_limited = 1;
		} else {
			sandbox->is_cpu_limited = 0;
		}
	}

	if (!zpp_emergency
		|| (Z_TYPE_PP(zpp_emergency) == IS_BOOL && Z_BVAL_PP(zpp_emergency) == 0))
	{
		// No emergency limit
		sandbox->cpu_emergency_limit.tv_sec = 0;
		sandbox->cpu_emergency_limit.tv_nsec = 0;
	} else {
		convert_scalar_to_number_ex(zpp_emergency);
		if (Z_TYPE_PP(zpp_emergency) == IS_LONG) {
			convert_to_double_ex(zpp_emergency);
		}
		if (Z_TYPE_PP(zpp_normal) == IS_DOUBLE) {
			luasandbox_set_timespec(&sandbox->cpu_emergency_limit, Z_DVAL_PP(zpp_emergency));
		} else {
			sandbox->cpu_emergency_limit.tv_sec = 0;
			sandbox->cpu_emergency_limit.tv_nsec = 0;
		}
	}
}
/* }}} */

/** {{{ luasandbox_set_timespec
 * Initialise a timespec structure with the time in seconds given by the source
 * argument.
 */
static void luasandbox_set_timespec(struct timespec * dest, double source)
{
	double fractional, integral;
	if (source < 0) {
		dest->tv_sec = dest->tv_nsec = 0;
		return;
	}

	fractional = modf(source, &integral);
	dest->tv_sec = (time_t)integral;
	dest->tv_nsec = (long)(fractional * 1000000000.0);
	if (dest->tv_nsec >= 1000000000L) {
		dest->tv_nsec -= 1000000000L;
		dest->tv_sec ++;
	}
}

/* }}} */

/** {{{ LuaSandbox::getMemoryUsage */
PHP_METHOD(LuaSandbox, getMemoryUsage)
{
	php_luasandbox_obj * sandbox = (php_luasandbox_obj*)
		zend_object_store_get_object(getThis() TSRMLS_CC);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		RETURN_FALSE;
	}

	RETURN_LONG(sandbox->alloc.memory_usage);
}
/* }}} */

/** {{{ proto array LuaSandbox::callFunction(string name, ...)
 *
 * Call a function in the global variable with the given name. The name may 
 * contain "." characters, in which case the function is located via recursive
 * table accesses, as if the name were a Lua expression.
 *
 * If the variable does not exist, or is not a function, false will be returned
 * and a warning issued.
 *
 * Any arguments specified after the name will be passed through as arguments 
 * to the function. 
 *
 * For more information about calling Lua functions and the return values, see
 * LuaSandboxFunction::call().
 */
PHP_METHOD(LuaSandbox, callFunction)
{
	char *name;
	int nameLength = 0;
	zend_uint numArgs = 0;
	zval *** args = NULL;

	php_luasandbox_obj * sandbox = (php_luasandbox_obj*)
		zend_object_store_get_object(getThis() TSRMLS_CC);
	lua_State * L = sandbox->state;
	CHECK_VALID_STATE(L);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s*",
		&name, &nameLength, &args, &numArgs) == FAILURE)
	{
		RETURN_FALSE;
	}

	// Find the function
	if (!luasandbox_find_field(L, LUA_GLOBALSINDEX, name, nameLength)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"The specified lua function does not exist");
		RETVAL_FALSE;
	} else {
		// Call it
		luasandbox_call_helper(L, getThis(), sandbox, args, numArgs, return_value TSRMLS_CC);
	}

	// Delete varargs
	if (numArgs) {
		efree(args);
	}
}
/* }}} */

/** {{{ luasandbox_function_init
 *
 * Common initialisation for LuaSandboxFunction methods. Initialise the
 * function, state and sandbox pointers, and push the function to the stack.
 */
static int luasandbox_function_init(zval * this_ptr, php_luasandboxfunction_obj ** pfunc, 
	lua_State ** pstate, php_luasandbox_obj ** psandbox TSRMLS_DC)
{
	*pfunc = (php_luasandboxfunction_obj *)
		zend_object_store_get_object(this_ptr TSRMLS_CC);
	if (!*pfunc || !(*pfunc)->sandbox || !(*pfunc)->index) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
			"attempt to call uninitialized LuaSandboxFunction object" );
		return 0;
	}

	*psandbox = (php_luasandbox_obj*)
		zend_object_store_get_object((*pfunc)->sandbox TSRMLS_CC);
	*pstate = (*psandbox)->state;

	if (!*pstate) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid LuaSandbox state");
		return 0;
	}

	// Find the function
	lua_getfield(*pstate, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
	lua_rawgeti(*pstate, -1, (*pfunc)->index);

	// Remove the table from the stack
	lua_remove(*pstate, -2);

	return 1;
}
/* }}} */

/* {{{ proto private final LuaSandboxFunction::__construct()
 *
 * Construct a LuaSandboxFunction object. Do not call this directly, use 
 * LuaSandbox::loadString().
 */
PHP_METHOD(LuaSandboxFunction, __construct)
{
	php_error_docref(NULL TSRMLS_CC, E_ERROR, "LuaSandboxFunction cannot be constructed directly");
}
/* }}} */

/** {{{ proto array LuaSandboxFunction::call(...)
 *
 * Call a LuaSandboxFunction. The arguments to this function are passed through
 * to Lua.
 *
 * Errors considered to be the fault of the PHP code will result in the 
 * function returning false and E_WARNING being raised, for example, a
 * resource type being used as an argument. Lua errors will result in a 
 * LuaSandboxError exception being thrown.
 *
 * Lua functions inherently return a list of results. So on success, this 
 * function returns an array containing all of the values returned by Lua, 
 * with integer keys starting from zero. Lua may return no results, in which 
 * case an empty array is returned.
 */
PHP_METHOD(LuaSandboxFunction, call)
{
	zend_uint numArgs = 0;
	zval *** args = NULL;
	
	php_luasandboxfunction_obj * func;
	lua_State * L;
	php_luasandbox_obj * sandbox;

	if (!luasandbox_function_init(getThis(), &func, &L, &sandbox TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "*",
		&args, &numArgs) == FAILURE)
	{
		RETURN_FALSE;
	}

	// Call the function
	luasandbox_call_helper(L, func->sandbox, sandbox, args, numArgs, return_value TSRMLS_CC);

	// Delete varargs
	if (numArgs) {
		efree(args);
	}
}
/** }}} */

/** {{{ luasandbox_call_helper
 *
 * Call the function at the top of the stack and then pop it. Set return_value
 * to an array containing all the results.
 */
static void luasandbox_call_helper(lua_State * L, zval * sandbox_zval, php_luasandbox_obj * sandbox, 
	zval *** args, zend_uint numArgs, zval * return_value TSRMLS_DC)
{
	// Save the top position
	int origTop = lua_gettop(L);
	int status;
	int cpu_limited = 0;
	luasandbox_timer_set t;
	int i, numResults;
	zval * old_zval;

	// Check to see if the value is a valid function
	if (lua_type(L, -1) != LUA_TFUNCTION) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"the specified Lua value is not a valid function");
		lua_settop(L, origTop - 1);
		RETURN_FALSE;
	}

	// Push the arguments
	for (i = 0; i < numArgs; i++) {
		if (!luasandbox_push_zval(L, *(args[i]))) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"unable to convert argument %d to a lua value", i + 1);
			lua_settop(L, origTop - 1);
			RETURN_FALSE;
		}
	}

	// Initialise the CPU limit timer
	if (!sandbox->in_lua && sandbox->is_cpu_limited) {
		cpu_limited = 1;
		if (!LUASANDBOX_G(signal_handler_installed)) {
			luasandbox_timer_install_handler(&LUASANDBOX_G(old_handler));
		}
		luasandbox_timer_start(&t, sandbox, &sandbox->cpu_normal_limit, 
			&sandbox->cpu_emergency_limit);
	}

	// Save the current zval for later use in luasandbox_call_php. Restore it 
	// after execution finishes, to support re-entrancy.
	old_zval = sandbox->current_zval;
	sandbox->current_zval = sandbox_zval;

	// Call the function
	sandbox->in_lua++;
	status = lua_pcall(L, numArgs, LUA_MULTRET, 0);
	sandbox->in_lua--;
	sandbox->current_zval = old_zval;

	// Stop the timer
	if (cpu_limited) {
		luasandbox_timer_stop(&t);
	}
	if (sandbox->emergency_timed_out) {
		luasandbox_handle_emergency_timeout(sandbox);
		return;
	}

	// Handle normal errors
	if (status) {
		luasandbox_handle_error(L, status);
		lua_settop(L, origTop - 1);
		RETURN_FALSE;
	}

	// Calculate the number of results and create an array of that capacity
	numResults = lua_gettop(L) - origTop + 1;
	array_init_size(return_value, numResults);

	// Fill the array with the results
	for (i = 0; i < numResults; i++) {
		zval * element;
		MAKE_STD_ZVAL(element);
		luasandbox_lua_to_zval(element, L, origTop + i, sandbox_zval, NULL TSRMLS_CC);
		zend_hash_next_index_insert(Z_ARRVAL_P(return_value), 
			(void*)&element,
			sizeof(zval*), NULL);
	}

	// Balance the stack
	lua_settop(L, origTop - 1);
}
/* }}} */

/** {{{ luasandbox_find_field
 *
 * Given a string in the format "a.b.c.d" find the relevant variable in the 
 * table at the given stack position. If it is found, 1 is returned
 * and the variable will be pushed to the stack. If not, 0 is returned
 * and the stack will be in the original state.
 */
static int luasandbox_find_field(lua_State * L, int index, 
	char * spec, int specLength)
{
	int i;
	int tokenStart = 0;
	const int top = lua_gettop(L);

	// Put a copy of the input table into top+1, this will be the index for
	// the parent table in the loop.
	lua_pushvalue(L, index);

	spec = estrndup(spec, specLength);
	
	for (i = 0; i <= specLength; i++) {
		if (i == specLength || spec[i] == '.') {
			// Put the next item into top+2
			lua_pushlstring(L, spec + tokenStart, i - tokenStart);
			lua_rawget(L, top + 1);

			// Not found?
			if (lua_isnil(L, top + 2)) {
				// Remove the two items we put on the stack and return
				lua_pop(L, 2);
				efree(spec);
				return 0;
			}

			// Record this position
			tokenStart = i + 1;

			// Shift the new item down to top+1
			lua_remove(L, top+1);
		}
	}

	efree(spec);
	return 1;
}
/* }}} */

/** {{{ luasandbox_get_php_obj
 *
 * Get the object data for a lua state.
 */
php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L)
{
	php_luasandbox_obj * obj;
	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");
	obj = (php_luasandbox_obj*)lua_touserdata(L, -1);
	assert(obj != NULL);
	lua_pop(L, 1);
	return obj;
}
/* }}} */

/** {{{ proto void LuaSandbox::registerLibrary(string libname, array functions)
 *
 * Register a set of PHP functions as a Lua library, so that Lua can call the
 * relevant PHP code.
 *
 * The first parameter is the name of the library. In the Lua state, the global
 * variable of this name will be set to the table of functions.
 *
 * The second parameter is an array, where each key is a function name, and 
 * each value is a corresponding PHP callback.
 *
 * Both Lua and PHP allow functions to be called with any number of arguments. 
 * The parameters to the Lua function will be passed through to the PHP. A 
 * single value will always be returned to Lua, which is the return value from
 * the PHP function. If the PHP function does not return any value, Lua will 
 * see a return value of nil.
 */
PHP_METHOD(LuaSandbox, registerLibrary)
{
	lua_State * L = luasandbox_state_from_zval(getThis() TSRMLS_CC);
	char * libname = NULL;
	int libname_len = 0;
	zval * zfunctions = NULL;
	HashTable * functions;
	Bucket * p;

	CHECK_VALID_STATE(L);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa",
		&libname, &libname_len, &zfunctions) == FAILURE)
	{
		RETURN_FALSE;
	}

	functions = Z_ARRVAL_P(zfunctions);

	// Determine if the library exists already
	// Make a copy of the library name on the stack for rawset later
	lua_pushlstring(L, libname, libname_len);
	lua_pushvalue(L, -1);
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (lua_type(L, -1) != LUA_TNIL) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"library \"%s\" already exists", libname);
		RETURN_FALSE;
	}
	// Remove the nil
	lua_pop(L, 1);

	// Create the new table
	lua_createtable(L, 0, functions->nNumOfElements);

	for (p = functions->pListHead; p; p = p->pListNext) {
		// Push the key
		if (p->nKeyLength) {
			lua_pushlstring(L, p->arKey, p->nKeyLength - 1);
		} else {
			lua_pushinteger(L, p->h);
		}

		// Push the callback zval and create the closure
		luasandbox_push_zval_userdata(L, *(zval**)p->pData);
		lua_pushcclosure(L, luasandbox_call_php, 1);

		// Add it to the table
		lua_rawset(L, -3);
	}

	// Move the new table to the global namespace
	// The key is on the stack already
	lua_rawset(L, LUA_GLOBALSINDEX);
}
/* }}} */

/** {{{ luasandbox_call_php
 *
 * The Lua callback for calling PHP functions. See the doc comment on 
 * LuaSandbox::registerLibrary() for information about calling conventions.
 */
static int luasandbox_call_php(lua_State * L)
{
	php_luasandbox_obj * intern = luasandbox_get_php_obj(L);
	
	luasandbox_enter_php(L, intern);

	zval ** callback_pp = lua_touserdata(L, lua_upvalueindex(1));
	zval *retval_ptr = NULL;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	char *is_callable_error = NULL;
	int top = lua_gettop(L);
	int i;
	void **temp;
	zval **pointers;
	zval ***double_pointers;
	int num_results = 0;
	TSRMLS_FETCH();

	// Based on zend_parse_arg_impl()
	if (zend_fcall_info_init(*callback_pp, 0, &fci, &fcc, NULL, 
		&is_callable_error TSRMLS_CC) != SUCCESS)
	{
		// Handle errors similar to the way PHP does it: show a warning and 
		// return nil
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
			"to be a valid callback, %s", is_callable_error);
		efree(is_callable_error);
		lua_pushnil(L);
		luasandbox_leave_php(L, intern);
		return 1;
	}

	fci.retval_ptr_ptr = &retval_ptr;

	// Make an array of zval double-pointers to hold the arguments
	temp = ecalloc(top, sizeof(void*) * 2);
	double_pointers = (zval***)temp;
	pointers = (zval**)(temp + top);
	for (i = 0; i < top; i++ ) {
		MAKE_STD_ZVAL(pointers[i]);
		luasandbox_lua_to_zval(pointers[i], L, i + 1, intern->current_zval, NULL TSRMLS_CC);
		double_pointers[i] = &(pointers[i]);
	}

	// Initialise the fci. Use zend_fcall_info_args_restore() since that's an
	// almost-legitimate way to avoid the extra malloc that we'd get from
	// zend_fcall_info_argp()
	zend_fcall_info_args_restore(&fci, top, double_pointers);

	// Call the function
	if (zend_call_function(&fci, &fcc TSRMLS_CC) == SUCCESS 
		&& fci.retval_ptr_ptr && *fci.retval_ptr_ptr) 
	{
		// Push the return value back to Lua
		luasandbox_push_zval(L, *fci.retval_ptr_ptr);
		num_results = 1;
		zval_ptr_dtor(&retval_ptr);
	}

	// Free the argument zvals
	for (i = 0; i < top; i++) {
		zval_ptr_dtor(&(pointers[i]));
	}

	// Free the pointer arrays
	efree(temp);
	luasandbox_leave_php(L, intern);

	// If an exception occurred, convert it to a Lua error (just to unwind the stack)
	if (EG(exception)) {
		luaL_error(L, "[exception]");
	}
	return num_results;
}
/* }}} */

/** {{{ string LuaSandboxFunction::dump()
 *
 * Dump the function as a precompiled binary blob. Returns a string which may
 * later be loaded by LuaSandbox::loadBinary(), in the same or a different 
 * sandbox object.
 */
PHP_METHOD(LuaSandboxFunction, dump)
{
	php_luasandboxfunction_obj * func;
	lua_State * L;
	php_luasandbox_obj * sandbox;
	smart_str buf = {0};

	if (!luasandbox_function_init(getThis(), &func, &L, &sandbox TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}
	
	lua_dump(L, luasandbox_dump_writer, (void*)&buf);
	smart_str_0(&buf);
	if (buf.len) {
		RETURN_STRINGL(buf.c, buf.len, 0);
	} else {
		smart_str_free(&buf);
		RETURN_EMPTY_STRING();
	}
}
/* }}} */

/** {{{ luasandbox_dump_writer
 *
 * Writer function for LuaSandboxFunction::dump().
 */
static int luasandbox_dump_writer(lua_State * L, const void * p, size_t sz, void * ud)
{
	smart_str * buf = (smart_str *)ud;
	smart_str_appendl(buf, p, sz);
	return 0;
}
/* }}} */
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
