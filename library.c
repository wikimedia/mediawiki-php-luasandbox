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
#include <math.h>

#include "php.h"
#include "php_luasandbox.h"

#ifdef LUASANDBOX_NO_CLOCK
#include <time.h>
#endif

static void luasandbox_lib_filter_table(lua_State * L, char ** member_names);
static HashTable * luasandbox_lib_get_allowed_globals();

static int luasandbox_base_tostring(lua_State * L);
static int luasandbox_math_random(lua_State * L);
static int luasandbox_math_randomseed(lua_State * L);
static int luasandbox_base_pcall(lua_State * L);
static int luasandbox_base_xpcall(lua_State *L);
static int luasandbox_os_clock(lua_State * L);

#if LUA_VERSION_NUM < 502
static int luasandbox_base_pairs(lua_State *L);
static int luasandbox_base_ipairs(lua_State *L);
#endif

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
	"math",
	"os",
	"debug",
	NULL
};

char * luasandbox_allowed_os_members[] = {
	"date",
	"difftime",
	"time",
	NULL
};

char * luasandbox_allowed_debug_members[] = {
	"traceback",
	NULL
};



ZEND_EXTERN_MODULE_GLOBALS(luasandbox);

/** {{{  luasandbox_lib_register
 */
void luasandbox_lib_register(lua_State * L)
{
	// Load the standard libraries that we need
	lua_pushcfunction(L, luaopen_base);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_table);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_math);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_debug);
	lua_call(L, 0, 0);
	lua_pushcfunction(L, luaopen_os);
	lua_call(L, 0, 0);

	// Install our own string library
	lua_pushcfunction(L, luasandbox_open_string);
	lua_call(L, 0, 0);

	// Filter the os library
	lua_getglobal(L, "os");
	luasandbox_lib_filter_table(L, luasandbox_allowed_os_members);
	lua_setglobal(L, "os");

	// Filter the debug library
	lua_getglobal(L, "debug");
	luasandbox_lib_filter_table(L, luasandbox_allowed_debug_members);
	lua_setglobal(L, "debug");

	// Remove any globals that aren't in a whitelist. This is mostly to remove
	// unsafe functions from the base library.
	lua_pushnil(L);
	while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
		const char * key;
		size_t key_len;
		lua_pop(L, 1);
		if (lua_type(L, -1) != LUA_TSTRING) {
			continue;
		}
		key = lua_tolstring(L, -1, &key_len);
		if (!zend_hash_str_exists(luasandbox_lib_get_allowed_globals(), key, key_len)) {
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

	// Install our own version of os.clock(), which uses our high-resolution
	// usage timer
	lua_getglobal(L, "os");
	lua_pushcfunction(L, luasandbox_os_clock);
	lua_setfield(L, -2, "clock");
	lua_pop(L, 1);

	// Install our own versions of pairs and ipairs, if necessary
#if LUA_VERSION_NUM < 502
	lua_getfield(L, LUA_GLOBALSINDEX, "pairs");
	lua_setfield(L, LUA_REGISTRYINDEX, "luasandbox_old_pairs");
	lua_getfield(L, LUA_GLOBALSINDEX, "ipairs");
	lua_setfield(L, LUA_REGISTRYINDEX, "luasandbox_old_ipairs");
	lua_pushcfunction(L, luasandbox_base_pairs);
	lua_setglobal(L, "pairs");
	lua_pushcfunction(L, luasandbox_base_ipairs);
	lua_setglobal(L, "ipairs");
#endif
}
/* }}} */

/** {{{ luasandbox_lib_filter_table
 *
 * Make a copy of the table at the top of the stack, and remove any members
 * from it that aren't in the given whitelist.
 */
static void luasandbox_lib_filter_table(lua_State * L, char ** member_names)
{
	int i, n;
	int si = lua_gettop(L);
	for (n = 0; member_names[n]; n++);
	lua_createtable(L, 0, n);
	for (i = 0; member_names[i]; i++) {
		lua_getfield(L, si, member_names[i]);
		lua_setfield(L, si+1, member_names[i]);
	}
	lua_replace(L, si);
}
/* }}} */

/** {{{ luasandbox_lib_destroy_globals */
void luasandbox_lib_destroy_globals()
{
	if (LUASANDBOX_G(allowed_globals)) {
		zend_hash_destroy(LUASANDBOX_G(allowed_globals));
		FREE_HASHTABLE(LUASANDBOX_G(allowed_globals));
		LUASANDBOX_G(allowed_globals) = NULL;
	}
}
/* }}} */

/** {{{ luasandbox_lib_get_allowed_globals
 *
 * Get a HashTable of allowed global variables
 */
static HashTable * luasandbox_lib_get_allowed_globals()
{
	int i, n;
	if (LUASANDBOX_G(allowed_globals)) {
		return LUASANDBOX_G(allowed_globals);
	}

	for (n = 0; luasandbox_allowed_globals[n]; n++);

	ALLOC_HASHTABLE(LUASANDBOX_G(allowed_globals));
	zend_hash_init(LUASANDBOX_G(allowed_globals), n, NULL, NULL, 0);

	zval zv;
	ZVAL_TRUE(&zv);

	for (i = 0; luasandbox_allowed_globals[i]; i++) {
		zend_hash_str_update(LUASANDBOX_G(allowed_globals),
			luasandbox_allowed_globals[i], strlen(luasandbox_allowed_globals[i]), &zv);
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

#ifdef PHP_WIN32
	// MSVC does not provide rand_r(). Note that srand/rand still does not
	// share state with PHP's rand() because PHP's rand() is an alias for mt_rand()
	// (and does not use C srand/rand) as of PHP 7.1.
	int i = rand();
#else
	int i = rand_r(&sandbox->random_seed);
#endif

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
#ifdef PHP_WIN32
	srand(sandbox->random_seed);
#endif
	return 0;
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

/** {{{ luasandbox_os_clock
 *
 * Implementation of os.clock() which uses our high-resolution usage timer,
 * if available, to provide an accurate measure of Lua CPU usage since a
 * particular LuaSandbox object was created.
 */
static int luasandbox_os_clock(lua_State * L)
{
	double clk;

#ifdef LUASANDBOX_NO_CLOCK
	clk = ((double)clock())/(double)CLOCKS_PER_SEC;
#else
	struct timespec ts;
	php_luasandbox_obj * sandbox = luasandbox_get_php_obj(L);
	luasandbox_timer_get_usage(&sandbox->timer, &ts);
	clk = ts.tv_sec + 1e-9 * ts.tv_nsec;
#endif

	// Reduce precision to 20μs to mitigate timing attacks
	clk = round(clk * 50000) / 50000;

	lua_pushnumber(L, (lua_Number)clk);
	return 1;
}

/* }}} */

#if LUA_VERSION_NUM < 502
/** {{{ luasandbox_base_pairs
 *
 * This is our implementation of the Lua function pairs(). It allows the Lua
 * 5.2 __pairs metamethod to override the standard behavior.
 */
static int luasandbox_base_pairs (lua_State *L)
{
	if (!luaL_getmetafield(L, 1, "__pairs")) {
		luaL_checktype(L, 1, LUA_TTABLE);
		lua_getfield(L, LUA_REGISTRYINDEX, "luasandbox_old_pairs");
	}
	lua_pushvalue(L, 1);
	lua_call(L, 1, 3);
	return 3;
}
/* }}} */

/** {{{ luasandbox_base_ipairs
 *
 * This is our implementation of the Lua function ipairs(). It allows the Lua
 * 5.2 __ipairs metamethod to override the standard behavior.
 */
static int luasandbox_base_ipairs (lua_State *L)
{
	if (!luaL_getmetafield(L, 1, "__ipairs")) {
		luaL_checktype(L, 1, LUA_TTABLE);
		lua_getfield(L, LUA_REGISTRYINDEX, "luasandbox_old_ipairs");
	}
	lua_pushvalue(L, 1);
	lua_call(L, 1, 3);
	return 3;
}
/* }}} */
#endif
