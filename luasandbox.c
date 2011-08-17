
/*
 * To do:
 *   * Provide a LuaSandbox::doString() as a convenience wrapper for 
 *     LuaSandbox::loadString()->call(), analogous to luaL_dostring().
 *
 *   * Add a PHP method for registering callback functions individually.
 *
 *   * Provide a wrapper around the base library, which allows only a whitelist
 *     of functions, not including e.g. loadfile(). This is slightly tricky 
 *     because the functions are not exported from the shared library. It's 
 *     necessary to call luaopen_base() on a separate lua_State, and then to 
 *     copy each function pointer to the destination lua_State using 
 *     lua_tocfunction().
 *
 *   * Support lua_dump()
 *   * Protect LuaSandbox->loadString() against malicious binaries
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <signal.h>
#include <time.h>

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "php_luasandbox.h"
#include "luasandbox_timer.h"

static zend_object_value luasandbox_new(zend_class_entry *ce TSRMLS_DC);
static lua_State * luasandbox_newstate(php_luasandbox_obj * intern);
static void luasandbox_free_storage(void *object TSRMLS_DC);
static zend_object_value luasandboxfunction_new(zend_class_entry *ce TSRMLS_CC);
static void luasandboxfunction_free_storage(void *object TSRMLS_DC);
static void luasandboxfunction_destroy(void *object, zend_object_handle handle TSRMLS_DC);
static int luasandbox_free_zval_userdata(lua_State * L);
static void *luasandbox_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
static int luasandbox_panic(lua_State * L);
static lua_State * luasandbox_state_from_zval(zval * this_ptr TSRMLS_DC);
static int luasandbox_find_field(lua_State * L, int index, 
	char * spec, int specLength);
static void luasandbox_set_timespec(struct timespec * dest, double source);
static void luasandbox_call_helper(lua_State * L, php_luasandbox_obj * sandbox,
	zval *** args, zend_uint numArgs, zval * return_value TSRMLS_DC);
static int luasandbox_push_zval(lua_State * L, zval * z);
static void luasandbox_push_zval_userdata(lua_State * L, zval * z);
static zval * luasandbox_lua_to_zval(lua_State * L, int index, HashTable * recursionGuard);
static void luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	HashTable * recursionGuard);
static php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L);
static void luasandbox_handle_error(lua_State * L, int status);
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht);
static int luasandbox_call_php(lua_State * L);

char luasandbox_timeout_message[] = "The maximum execution time for this script was exceeded";

zend_class_entry *luasandbox_ce;
zend_class_entry *luasandboxerror_ce;
zend_class_entry *luasandboxplaceholder_ce;
zend_class_entry *luasandboxfunction_ce;

ZEND_DECLARE_MODULE_GLOBALS(luasandbox);

/** {{{ arginfo */
ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_loadString, 0)
	ZEND_ARG_INFO(0, code)
	ZEND_ARG_INFO(0, chunkName)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setMemoryLimit, 0)
	ZEND_ARG_INFO(0, limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setCPULimit, 0)
	ZEND_ARG_INFO(0, normal_limit)
	ZEND_ARG_INFO(0, emergency_limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_callFunction, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, ...)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_register, 0)
	ZEND_ARG_INFO(0, libname)
	ZEND_ARG_INFO(0, functions)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandboxfunction_call, 0)
	ZEND_ARG_INFO(0, ...)
ZEND_END_ARG_INFO()
/* }}} */

/** {{{ function entries */
const zend_function_entry luasandbox_functions[] = {
	{NULL, NULL, NULL}	/* Must be the last line in luasandbox_functions[] */
};

const zend_function_entry luasandbox_methods[] = {
	PHP_ME(LuaSandbox, loadString, arginfo_luasandbox_loadString, 0)
	PHP_ME(LuaSandbox, setMemoryLimit, arginfo_luasandbox_setMemoryLimit, 0)
	PHP_ME(LuaSandbox, setCPULimit, arginfo_luasandbox_setCPULimit, 0)
	PHP_ME(LuaSandbox, callFunction, arginfo_luasandbox_callFunction, 0)
	PHP_ME(LuaSandbox, register, arginfo_luasandbox_register, 0)
	{NULL, NULL, NULL}
};

const zend_function_entry luasandboxfunction_methods[] = {
	PHP_ME(LuaSandboxFunction, call, arginfo_luasandboxfunction_call, 0)
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
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(luasandbox)
{
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

/** {{{ luasandbox_enter_php
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
	intern->in_php = 1;
	if (intern->timed_out) {
		intern->in_php = 0;
		luaL_error(L, luasandbox_timeout_message);
	}
}
/* }}} */

/** {{{ luasandbox_leave_php
 */
static inline void luasandbox_leave_php(lua_State * L, php_luasandbox_obj * intern)
{
	intern->in_php = 0;
}
/* }}} */

/** {{{ luasandbox_new */
static zend_object_value luasandbox_new(zend_class_entry *ce TSRMLS_DC)
{
	php_luasandbox_obj * intern;
	zend_object_value retval;

	// Create the internal object
	intern = emalloc(sizeof(php_luasandbox_obj));
	memset(intern, 0, sizeof(php_luasandbox_obj));
	zend_object_std_init(&intern->std, ce TSRMLS_CC);
	
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

/** {{{ luasandbox_newstate */
static lua_State * luasandbox_newstate(php_luasandbox_obj * intern) 
{
	lua_State * L;

	L = lua_newstate(luasandbox_alloc, intern);
	lua_atpanic(L, luasandbox_panic);

	// Load some relatively safe standard libraries
	luaopen_string(L);
	luaopen_table(L);
	luaopen_math(L);

	// Create a table for storing chunks
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
	
	// Register a pointer to the PHP object so that C functions can find it
	lua_pushlightuserdata(L, (void*)intern);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");
	
	// Create the metatable for zval destruction
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, luasandbox_free_zval_userdata);
	lua_setfield(L, -2, "__gc");
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_zval_metatable");

	return L;
}
/* }}} */

/** {{{ luasandbox_free_storage */
static void luasandbox_free_storage(void *object TSRMLS_DC)
{
	php_luasandbox_obj * intern = (php_luasandbox_obj*)object;
	lua_close(intern->state);
	intern->state = NULL;
	zend_object_std_dtor(&intern->std);
	efree(object);
}
/* }}} */

/** {{{ luasandboxfunction_new */
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

/** {{{ luasandboxfunction_destroy */
static void luasandboxfunction_destroy(void *object, zend_object_handle handle TSRMLS_DC)
{
	php_luasandboxfunction_obj * func = (php_luasandboxfunction_obj*)object;
	if (func->sandbox) {
		php_luasandbox_obj * sandbox = (php_luasandbox_obj*)
			zend_object_store_get_object(func->sandbox TSRMLS_CC);
		lua_State * L = sandbox->state;

		// Delete the chunk
		if (func->index) {
			lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
			lua_pushnil(L);
			lua_rawseti(L, -2, func->index);
			lua_pop(L, 1);
		}

		// Delete the parent reference
		zval_ptr_dtor(&func->sandbox);
	}
}
/* }}} */

/** {{{ luasandboxfunction_free_storage */
static void luasandboxfunction_free_storage(void *object TSRMLS_DC)
{
	php_luasandboxfunction_obj * func = (php_luasandboxfunction_obj*)object;
	zend_object_std_dtor(&func->std);
	efree(object);
}
/* }}} */

/** {{{ luasandbox_free_zval_userdata
 * Free a zval given to Lua by luasandbox_push_zval_userdata.
 */
static int luasandbox_free_zval_userdata(lua_State * L)
{
	zval ** zpp = (zval**)lua_touserdata(L, 1);
	php_luasandbox_obj * intern = luasandbox_get_php_obj(L);
	luasandbox_enter_php(L, intern);
	if (zpp && *zpp) {
		zval_ptr_dtor(zpp);
	}
	*zpp = NULL;
	luasandbox_leave_php(L, intern);
	return 0;
}
/* }}} */

/** {{{ luasandbox_alloc */
static void *luasandbox_alloc(void *ud, void *ptr, size_t osize, size_t nsize) 
{
	php_luasandbox_obj * obj = (php_luasandbox_obj*)ud;
	void * nptr;
	obj->in_php = 1;

	// Update memory usage accounting
	if (obj->memory_usage + nsize < obj->memory_usage) {
		// Overflow
		// No PHP error because it's plausible that untrusted scripts could do this.
		obj->in_php = 0;
		return NULL;
	}

	obj->memory_usage += nsize - osize;

	if (nsize == 0) {
		if (ptr) {
			efree(ptr);
		}
		nptr = NULL;
	} else if (osize == 0) {
		nptr = emalloc(nsize);
	} else {
		nptr = erealloc(ptr, nsize);
	}
	obj->in_php = 0;
	return nptr;
}
/* }}} */

/** {{{ luasandbox_panic */
static int luasandbox_panic(lua_State * L)
{
	php_error_docref(NULL TSRMLS_CC, E_ERROR,
		"PANIC: unprotected error in call to Lua API (%s)",
		lua_tostring(L, -1));
	return 0;
}
/* }}} */

/** {{{ luasandbox_state_from_zval */
static lua_State * luasandbox_state_from_zval(zval * this_ptr TSRMLS_DC)
{
	php_luasandbox_obj * intern = (php_luasandbox_obj*) 
		zend_object_store_get_object(this_ptr TSRMLS_CC);
	return intern->state;
}
/* }}} */

/** {{{ proto int LuaSandbox::loadString(string code, string chunkName) */
PHP_METHOD(LuaSandbox, loadString)
{
	char *code, *chunkName = NULL;
	int codeLength, chunkNameLength;
	int status;
	lua_State * L = luasandbox_state_from_zval(getThis() TSRMLS_CC);
	size_t index;
	php_luasandboxfunction_obj * func_obj;

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

	// Get the chunks table
	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");

	// Get the next free index
	index = lua_objlen(L, -1);
	if (index >= INT_MAX) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"too many chunks loaded already");
		RETURN_FALSE;
	}
	index++;

	// Parse the string into a function on the stack
	status = luaL_loadbuffer(L, code, codeLength, chunkName);
	if (status != 0) {
		luasandbox_handle_error(L, status);
		return;
	}
	
	// Store the resulting function to the chunks table
	lua_rawseti(L, -2, (int)index);

	// Create a LuaSandboxFunction object to hold a reference to the function
	object_init_ex(return_value, luasandboxfunction_ce);
	func_obj = (php_luasandboxfunction_obj*)zend_object_store_get_object(return_value);
	func_obj->index = index;
	func_obj->sandbox = getThis();
	Z_ADDREF_P(getThis());

	// Balance the stack
	lua_pop(L, 1);
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
	zend_throw_exception(luasandboxerror_ce, (char*)errorMsg, status);
}
/* }}} */

/** {{{ proto void LuaSandbox::setMemoryLimit(int limit) */
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

	intern->memory_limit = limit;
}
/* }}} */


/** {{{ proto void LuaSandbox::setCPULimit(mixed normal_limit, mixed emergency_limit = false) */
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

/** {{{ luasandbox_set_timespec */
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

/*** {{{ proto array LuaSandbox::callFunction(string name, ...)
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
		luasandbox_call_helper(L, sandbox, args, numArgs, return_value TSRMLS_CC);
	}

	// Delete varargs
	if (numArgs) {
		efree(args);
	}
}
/* }}} */

/*** {{{ proto array LuaSandboxFunction::call(...)
 */
PHP_METHOD(LuaSandboxFunction, call)
{
	zend_uint numArgs = 0;
	zval *** args = NULL;
	
	php_luasandboxfunction_obj * func = (php_luasandboxfunction_obj *)
		zend_object_store_get_object(getThis() TSRMLS_CC);
	lua_State * L;
	php_luasandbox_obj * sandbox;

	if (!func || !func->sandbox || !func->index) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
			"attempt to call uninitialized LuaSandboxFunction object" );
		RETURN_FALSE;
	}

	sandbox = (php_luasandbox_obj*)
		zend_object_store_get_object(func->sandbox TSRMLS_CC);
	L = sandbox->state;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "*",
		&args, &numArgs) == FAILURE)
	{
		RETURN_FALSE;
	}

	// Find the function
	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
	lua_rawgeti(L, -1, func->index);

	// Call it
	luasandbox_call_helper(L, sandbox, args, numArgs, return_value TSRMLS_CC);

	// Delete varargs
	if (numArgs) {
		efree(args);
	}
}
/** }}} */

/** {{{ luasandbox_call_helper
 * Call the function at the top of the stack and then pop it. Set return_value
 * to an array containing all the results.
 */
static void luasandbox_call_helper(lua_State * L, php_luasandbox_obj * sandbox, 
	zval *** args, zend_uint numArgs, zval * return_value TSRMLS_DC)
{
	// Save the top position
	int origTop = lua_gettop(L);
	int status;
	int cpu_limited = 0;
	luasandbox_timer_set t;
	int i, numResults;

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
	if (sandbox->is_cpu_limited) {
		cpu_limited = 1;
		if (!LUASANDBOX_G(signal_handler_installed)) {
			luasandbox_timer_install_handler(&LUASANDBOX_G(old_handler));
		}
		luasandbox_timer_start(&t, sandbox, &sandbox->cpu_normal_limit, 
			&sandbox->cpu_emergency_limit);
	}

	// Call the function
	status = lua_pcall(L, numArgs, LUA_MULTRET, 0);

	// Stop the timer
	if (cpu_limited) {
		luasandbox_timer_stop(&t);
	}

	// If there was an emergency timeout, destroy the state
	if (sandbox->emergency_timed_out) {
		lua_close(L);
		L = sandbox->state = luasandbox_newstate(sandbox);
		sandbox->emergency_timed_out = 0;
		zend_throw_exception(luasandboxerror_ce, 
			"The maximum execution time was exceeded "
			"and the current Lua statement failed to return, leading to "
			"destruction of the Lua state", LUA_ERRRUN);
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
		zval * element = luasandbox_lua_to_zval(L, origTop + i, NULL);
		zend_hash_next_index_insert(Z_ARRVAL_P(return_value), 
			(void*)&element,
			sizeof(zval*), NULL);
	}

	// Balance the stack
	lua_settop(L, origTop - 1);
}
/* }}} */

/** {{{ luasandbox_find_field
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

/** {{{ luasandbox_push_zval
 * Convert a zval to an appropriate Lua type and push the resulting value on to
 * the stack.
 */
static int luasandbox_push_zval(lua_State * L, zval * z)
{
	switch (Z_TYPE_P(z)) {
		case IS_NULL:
			lua_pushnil(L);
			break;
		case IS_LONG:
			lua_pushinteger(L, Z_LVAL_P(z));
			break;
		case IS_DOUBLE:
			lua_pushnumber(L, Z_DVAL_P(z));
			break;
		case IS_BOOL:
			lua_pushboolean(L, Z_BVAL_P(z));
			break;
		case IS_ARRAY:
			if (!luasandbox_push_hashtable(L, Z_ARRVAL_P(z))) {
				return 0;
			}
			break;
		case IS_OBJECT:
			if (!luasandbox_push_hashtable(L, Z_OBJPROP_P(z))) {
				return 0;
			}
			break;
		case IS_STRING:
			lua_pushlstring(L, Z_STRVAL_P(z), Z_STRLEN_P(z));
			break;
		case IS_RESOURCE:
		case IS_CONSTANT:
		case IS_CONSTANT_ARRAY:
		default:
			return 0;
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_push_zval_userdata
 * Push a full userdata on to the stack, which stores a zval* in its block. 
 * Increment its reference count and set its metatable so that it will be freed 
 * at the appropriate time.
 */
static void luasandbox_push_zval_userdata(lua_State * L, zval * z)
{
	zval ** ud;
	ud = (zval**)lua_newuserdata(L, sizeof(zval*));
	*ud = z;
	Z_ADDREF_P(z);

	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_zval_metatable");
	lua_setmetatable(L, -2);
}
/* }}} */

/** {{{ luasandbox_push_hashtable */
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht)
{
	Bucket * p;

	// Recursion requires an arbitrary amount of stack space so we have to 
	// check the stack.
	luaL_checkstack(L, 10, "converting PHP array to Lua");

	lua_newtable(L);
	if (!ht || !ht->nNumOfElements) {
		return 1;
	}
	if (ht->nApplyCount) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "recursion detected");
		return 0;
	}
	ht->nApplyCount++;
	for (p = ht->pListHead; p; p = p->pListNext) {
		if (p->nKeyLength) {
			lua_pushlstring(L, p->arKey, p->nKeyLength - 1);
		} else {
			lua_pushinteger(L, p->h);
		}
		
		if (!luasandbox_push_zval(L, *(zval**)p->pData)) {
			// Failed to process that data value
			// Pop the key and the half-constructed table
			lua_pop(L, 2);
			ht->nApplyCount--;
			return 0;
		}

		lua_settable(L, -3);
	}
	ht->nApplyCount--;
	return 1;
}
/* }}} */

/** {{{ luasandbox_lua_to_zval
 *
 * Convert a lua value to a zval. Allocates the zval on the heap and returns 
 * a pointer to it.
 *
 * If a value is encountered that can't be converted to a zval, a LuaPlaceholder
 * object is returned instead.
 *
 * @param L The lua state
 * @param index The stack index to the input value
 * @param recursionGuard A hashtable for keeping track of tables that have been 
 *     processed, to allow infinite recursion to be avoided. External callers 
 *     should set this to NULL.
 */
static zval * luasandbox_lua_to_zval(lua_State * L, int index, HashTable * recursionGuard)
{
	zval * z;
	MAKE_STD_ZVAL(z);

	switch (lua_type(L, index)) {
		case LUA_TNIL:
			ZVAL_NULL(z);
			break;
		case LUA_TNUMBER: {
			long i;
			double d, integerPart, fractionalPart;
			// Lua only provides a single number type
			// Convert it to a PHP integer if that can be done without loss
			// of precision
			d = lua_tonumber(L, index);
			fractionalPart = modf(d, &integerPart);
			if (fractionalPart == 0.0 && integerPart >= LONG_MIN && integerPart <= LONG_MAX) {
				// The number is small enough to fit inside an int. But has it already
				// been truncated by squeezing it into a double? This is only relevant
				// where the integer size is greater than the mantissa size.
				i = (long)integerPart;
				if (LONG_MAX < (1LL << DBL_MANT_DIG)
					|| labs(i) < (1L << DBL_MANT_DIG))
				{
					ZVAL_LONG(z, i);
				} else {				
					ZVAL_DOUBLE(z, d);
				}
			} else {
				ZVAL_DOUBLE(z, d);
			}
			break;
		}
		case LUA_TBOOLEAN:
			ZVAL_BOOL(z, lua_toboolean(L, index));
			break;
		case LUA_TSTRING: {
			const char * str;
			size_t length;
			str = lua_tolstring(L, index, &length);
			ZVAL_STRINGL(z, str, length, 1);
			break;
		}
		case LUA_TTABLE: {
			const void * ptr = lua_topointer(L, index);
			void * data = NULL;
			int allocated = 0;
			if (recursionGuard) {
				// Check for circular reference (infinite recursion)
				if (zend_hash_find(recursionGuard, (char*)&ptr, sizeof(void*), &data) == SUCCESS) {
					// Found circular reference!
					object_init_ex(z, luasandboxplaceholder_ce);
					break;
				}
			} else {
				ALLOC_HASHTABLE(recursionGuard);
				zend_hash_init(recursionGuard, 1, NULL, NULL, 0);
				allocated = 1;
			}

			// Add the current table to the recursion guard hashtable
			// Use the pointer as the key, zero-length data
			zend_hash_update(recursionGuard, (char*)&ptr, sizeof(void*), &data, 0, NULL);

			// Process the array
			array_init(z);
			luasandbox_lua_to_array(Z_ARRVAL_P(z), L, index, recursionGuard);

			if (allocated) {
				zend_hash_destroy(recursionGuard);
				FREE_HASHTABLE(recursionGuard);
			}
			break;
		}
		case LUA_TFUNCTION:
		case LUA_TUSERDATA:
		case LUA_TTHREAD:
		case LUA_TLIGHTUSERDATA:
		default:
			// TODO: provide derived classes for each type
			object_init_ex(z, luasandboxplaceholder_ce);
	}
	return z;
}
/* }}} */

/** {{{ luasandbox_lua_to_array 
 * Append the elements of the table in the specified index to the given HashTable.
 */
static void luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	HashTable * recursionGuard)
{
	const char * str;
	size_t length;
	zval *value;

	// Normalise the input index so that we can push without invalidating it.
	if (index < 0) {
		index += lua_gettop(L) + 1;
	}

	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		value = luasandbox_lua_to_zval(L, -1, recursionGuard);
		
		// Make a copy of the key so that we can call lua_tolstring() which is destructive
		lua_pushvalue(L, -2);
		str = lua_tolstring(L, -1, &length);
		zend_hash_update(ht, str, length + 1, (void*)&value, sizeof(zval*), NULL);

		// Delete the copy and the value
		lua_pop(L, 2);
	}
}
/* }}} */

/** {{{ luasandbox_get_php_obj
 * Get the object data for a lua state.
 */
static php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L)
{
	php_luasandbox_obj * obj;
	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");
	obj = (php_luasandbox_obj*)lua_touserdata(L, -1);
	assert(obj != NULL);
	lua_pop(L, 1);
	return obj;
}
/* }}} */

/** {{{ proto void LuaSandbox::register(string libname, array functions)
 */
PHP_METHOD(LuaSandbox, register)
{
	lua_State * L = luasandbox_state_from_zval(getThis() TSRMLS_CC);
	char * libname = NULL;
	int libname_len = 0;
	zval * zfunctions = NULL;
	HashTable * functions;
	Bucket * p;

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
		pointers[i] = luasandbox_lua_to_zval(L, i + 1, NULL);
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
	}

	// Free the zvals
	for (i = 0; i < top; i++) {
		zval_ptr_dtor(&(pointers[i]));
	}
	zval_ptr_dtor(&retval_ptr);

	// Free the pointer arrays
	efree(temp);
	luasandbox_leave_php(L, intern);
	return num_results;
}
/* }}} */

/* }}} */
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
