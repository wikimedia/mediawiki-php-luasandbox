/**
 * This file holds the library of functions which are written in C and exposed 
 * to Lua code, and the code which manages registration of both the custom 
 * library and the parts of the standard Lua library which we allow.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "php.h"
#include "php_luasandbox.h"
#include "luasandbox_unicode.h"

static HashTable * luasandbox_lib_get_allowed_globals(TSRMLS_D);

static int luasandbox_base_tostring(lua_State * L);
static int luasandbox_math_random(lua_State * L);
static int luasandbox_math_randomseed(lua_State * L);
static int luasandbox_base_pcall(lua_State * L);
static int luasandbox_base_xpcall(lua_State *L);

/**
 * Allowed global variables. Omissions are:
 *   * pcall, xpcall: We have our own versions which don't allow interception of
 *     timeout etc. errors. 
 *   * loadfile: insecure.
 *   * load, loadstring: Probably creates a protected environment so has 
 *     the same problem as pcall. Also omitting these makes analysis of the 
 *     code for runtime etc. feasible.
 *   * print: Not compatible with a sandbox environment
 *   * tostring: Provides addresses of tables and functions, which provides an 
 *     easy ASLR workaround or heap address discovery mechanism for a memory 
 *     corruption exploit. We have our own version.
 *   * Any new or undocumented functions like newproxy.
 *   * package: cpath, loadlib etc. are insecure.
 *   * coroutine: Not useful for our application so unreviewed at present.
 *   * io, file, os: insecure
 *   * debug: Provides various ways to break the sandbox, such as setupvalue()
 *     and getregistry().
 */
char * luasandbox_allowed_globals[] = {
	// base
	"assert",
	"error",
	"getfenv",
	"getmetatable",
	"ipairs",
	"next",
	"pairs",
	"rawequal",
	"rawget",
	"rawset",
	"select",
	"setfenv",
	"setmetatable",
	"tonumber",
	"type",
	"unpack",
	"_G",
	"_VERSION",
	// libs
	"string",
	"table",
	"math"
};
#define LUASANDBOX_NUM_ALLOWED_GLOBALS (sizeof(luasandbox_allowed_globals) / sizeof(char*))

ZEND_EXTERN_MODULE_GLOBALS(luasandbox);

/** {{{  luasandbox_lib_register
 */
void luasandbox_lib_register(lua_State * L TSRMLS_DC)
{
	// Load some relatively safe standard libraries
	lua_pushcfunction(L, luaopen_base);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_string);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_table);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_math);
	lua_call(L, 0, 0);

	// Remove any globals that aren't in a whitelist. This is mostly to remove 
	// unsafe functions from the base library.
	lua_pushnil(L);
	while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
		const char * key;
		size_t key_len;
		void * data;
		lua_pop(L, 1);
		if (lua_type(L, -1) != LUA_TSTRING) {
			continue;
		}
		key = lua_tolstring(L, -1, &key_len);
		if (zend_hash_find(luasandbox_lib_get_allowed_globals(), 
			(char*)key, key_len + 1, &data) == FAILURE) 
		{
			// Not allowed, delete it
			lua_pushnil(L);
			lua_setglobal(L, key);
		}
	}

	// Install our own versions of tostring, pcall, xpcall
	lua_pushcfunction(L, luasandbox_base_tostring);
	lua_setglobal(L, "tostring");
	lua_pushcfunction(L, luasandbox_base_pcall);
	lua_setglobal(L, "pcall");
	lua_pushcfunction(L, luasandbox_base_xpcall);
	lua_setglobal(L, "xpcall");

	// Remove string.dump: may expose private data
	lua_getglobal(L, "string");
	lua_pushnil(L);
	lua_setfield(L, -2, "dump");
	lua_pop(L, 1);

	// Install our own versions of math.random and math.randomseed
	lua_getglobal(L, "math");
	lua_pushcfunction(L, luasandbox_math_random);
	lua_setfield(L, -2, "random");
	lua_pushcfunction(L, luasandbox_math_randomseed);
	lua_setfield(L, -2, "randomseed");
	lua_pop(L, 1);
	
	// Install string-related functions
	luasandbox_install_unicode_functions(L);
}
/* }}} */

/** {{{ luasandbox_lib_shutdown */
void luasandbox_lib_shutdown(TSRMLS_D)
{
	if (LUASANDBOX_G(allowed_globals)) {
		zend_hash_destroy(LUASANDBOX_G(allowed_globals));
		pefree(LUASANDBOX_G(allowed_globals), 1);
	}
}
/* }}} */

/** {{{ luasandbox_lib_get_allowed_globals
 *
 * Get a HashTable of allowed global variables
 */
static HashTable * luasandbox_lib_get_allowed_globals(TSRMLS_D)
{
	int i;
	if (LUASANDBOX_G(allowed_globals)) {
		return LUASANDBOX_G(allowed_globals);
	}

	LUASANDBOX_G(allowed_globals) = pemalloc(sizeof(HashTable), 1);
	zend_hash_init(LUASANDBOX_G(allowed_globals), LUASANDBOX_NUM_ALLOWED_GLOBALS, NULL, NULL, 1);
	for (i = 0; i < LUASANDBOX_NUM_ALLOWED_GLOBALS; i++) {
		zend_hash_update(LUASANDBOX_G(allowed_globals), 
			luasandbox_allowed_globals[i], strlen(luasandbox_allowed_globals[i]) + 1,
			"", 1, NULL);
	}

	return LUASANDBOX_G(allowed_globals);
}
/* }}} */

/** {{{ luasandbox_base_tostring
 *
 * This is identical to luaB_tostring except that it does not call lua_topointer().
 */
static int luasandbox_base_tostring(lua_State * L)
{
	luaL_checkany(L, 1);
	if (luaL_callmeta(L, 1, "__tostring"))  /* is there a metafield? */
		return 1;  /* use its value */
	switch (lua_type(L, 1)) {
		case LUA_TNUMBER:
			lua_pushstring(L, lua_tostring(L, 1));
			break;
		case LUA_TSTRING:
			lua_pushvalue(L, 1);
			break;
		case LUA_TBOOLEAN:
			lua_pushstring(L, (lua_toboolean(L, 1) ? "true" : "false"));
			break;
		case LUA_TNIL:
			lua_pushliteral(L, "nil");
			break;
		default:
			lua_pushfstring(L, "%s", luaL_typename(L, 1));
			break;
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_math_random
 *
 * A math.random implementation that doesn't share state with PHP's rand()
 */
static int luasandbox_math_random(lua_State * L)
{
	php_luasandbox_obj * sandbox = luasandbox_get_php_obj(L);

	int i = rand_r(&sandbox->random_seed);
	if (i >= RAND_MAX) {
		i -= RAND_MAX;
	}
	lua_Number r = (lua_Number)i / (lua_Number)RAND_MAX;
	switch (lua_gettop(L)) {  /* check number of arguments */
		case 0: {  /* no arguments */
			lua_pushnumber(L, r);  /* Number between 0 and 1 */
			break;
		}
		case 1: {  /* only upper limit */
			int u = luaL_checkint(L, 1);
			luaL_argcheck(L, 1<=u, 1, "interval is empty");
			lua_pushnumber(L, floor(r*u)+1);  /* int between 1 and `u' */
			break;
		}
		case 2: {  /* lower and upper limits */
			int l = luaL_checkint(L, 1);
			int u = luaL_checkint(L, 2);
			luaL_argcheck(L, l<=u, 2, "interval is empty");
			lua_pushnumber(L, floor(r*(u-l+1))+l);  /* int between `l' and `u' */
			break;
		}
		default: return luaL_error(L, "wrong number of arguments");
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_math_randomseed
 *
 * Set the seed for the custom math.random.
 */
static int luasandbox_math_randomseed(lua_State * L)
{
	php_luasandbox_obj * sandbox = luasandbox_get_php_obj(L);
	sandbox->random_seed = (unsigned int)luaL_checkint(L, 1);
}
/* }}} */

/** {{{ luasandbox_lib_rethrow_fatal
 *
 * If the error on the top of the stack with the error return code given as a 
 * parameter is a fatal, rethrow the error. If the error is rethrown, the 
 * function does not return.
 */
static void luasandbox_lib_rethrow_fatal(lua_State * L, int status)
{
	switch (status) {
		case 0:
			// No error
			return;
		case LUA_ERRRUN:
			// A runtime error which we can rethrow in a normal way
			if (luasandbox_is_fatal(L, -1)) {
				lua_error(L);
			}
			break;
		case LUA_ERRMEM:
		case LUA_ERRERR:
			// Lua doesn't provide a public API for rethrowing these, so we 
			// have to convert them to our own fatal error type
			if (!luasandbox_is_fatal(L, -1)) {
				luasandbox_wrap_fatal(L);
			}
			lua_error(L);
			break; // not reached
	}
}
/* }}} */

/** {{{ luasandbox_lib_error_wrapper
 *
 * Wrapper for the xpcall error function
 */
static int luasandbox_lib_error_wrapper(lua_State * L)
{
	int status;
	luaL_checkany(L, 1);

	// This function is only called from luaG_errormsg(), which will later 
	// unconditionally set the status code to LUA_ERRRUN, so we can assume 
	// that the error type is equivalent to LUA_ERRRUN.
	if (luasandbox_is_fatal(L, 1)) {
		// Just return to whatever called lua_pcall(), don't call the user 
		// function
		return lua_gettop(L);
	}

	// Put the user error function at the bottom of the stack
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_insert(L, 1);
	// Call it, passing through the arguments to this function
	status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
	if (status != 0) {
		luasandbox_lib_rethrow_fatal(L, LUA_ERRERR);
	}
	return lua_gettop(L);
}
/* }}} */

/** {{{ luasandbox_base_pcall
 *
 * This is our implementation of the Lua function pcall(). It allows Lua code
 * to handle its own errors, but forces internal errors to be rethrown.
 */
static int luasandbox_base_pcall(lua_State * L)
{
	int status;
	luaL_checkany(L, 1);
	status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
	luasandbox_lib_rethrow_fatal(L, status);
	lua_pushboolean(L, (status == 0));
	lua_insert(L, 1);
	return lua_gettop(L);  // return status + all results
}
/* }}} */

/** {{{ luasandbox_base_xpcall
 *
 * This is our implementation of the Lua function xpcall(). It allows Lua code
 * to handle its own errors, but forces internal errors to be rethrown.
 */
static int luasandbox_base_xpcall (lua_State *L)
{
	int status;
	luaL_checkany(L, 2);
	lua_settop(L, 2);
	
	// We wrap the error function in a C closure. The error function already 
	// happens to be at the top of the stack, so we don't need to push it before
	// calling lua_pushcfunction to make it an upvalue
	lua_pushcclosure(L, luasandbox_lib_error_wrapper, 1);
	lua_insert(L, 1);  // put error function under function to be called

	status = lua_pcall(L, 0, LUA_MULTRET, 1);
	luasandbox_lib_rethrow_fatal(L, status);
	lua_pushboolean(L, (status == 0));
	lua_replace(L, 1);
	return lua_gettop(L);  // return status + all results
}
/* }}} */

