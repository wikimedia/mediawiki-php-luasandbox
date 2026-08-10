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
#include "luasandbox_lua_compat.h"

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
static int luasandbox_base_unpack(lua_State * L);

static int luasandbox_base_pairs(lua_State *L);
static int luasandbox_base_ipairs(lua_State *L);

#if LUA_VERSION_NUM >= 502
static int luasandbox_base_setfenv(lua_State *L);
static int luasandbox_base_getfenv(lua_State *L);
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
#if LUA_VERSION_NUM < 502
	"getfenv",
#endif
	"getmetatable",
	"ipairs",
	"next",
	"pairs",
	"rawequal",
	"rawget",
	"rawset",
	"select",
#if LUA_VERSION_NUM < 502
	"setfenv",
#endif
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

/**
 * Allowed members of the table library. The library is filtered rather than
 * exposed wholesale because its contents vary between the Lua versions
 * LuaSandbox supports, so a new version would otherwise silently add
 * unreviewed functions to the sandbox. Omissions are:
 *   * move: Copies an arbitrary range entirely within a C loop, so neither the
 *     instruction count hook nor the memory limit can interrupt it. A single
 *     table.move(t, 1, 2^40, 1, {}) ignores the CPU limit indefinitely.
 *   * create: Lua 5.5 and later, unreviewed at present.
 * Names not present in the Lua version being built against are ignored.
 */
char * luasandbox_allowed_table_members[] = {
	"concat",
	// 5.1 only
	"foreach",
	"foreachi",
	"getn",
	"insert",
	// 5.1 only
	"maxn",
	// 5.2+ only
	"pack",
	"remove",
	// 5.1 only
	"setn",
	"sort",
	// 5.2+ only
	"unpack",
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
	luasandbox_luaL_requiref(L, LUA_GNAME, luaopen_base);
	luasandbox_luaL_requiref(L, LUA_TABLIBNAME, luaopen_table);
	luasandbox_luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math);
	luasandbox_luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug);
	luasandbox_luaL_requiref(L, LUA_OSLIBNAME, luaopen_os);

	// Install our own string library
	lua_pushcfunction(L, luasandbox_open_string);
	lua_call(L, 0, 0);

	// Filter the table library
	lua_getglobal(L, "table");
	luasandbox_lib_filter_table(L, luasandbox_allowed_table_members);
	lua_setglobal(L, "table");

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
	luasandbox_pushglobaltable(L);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
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
	lua_pop(L, 1);

	// Install our own versions of tostring, pcall, xpcall, unpack
	lua_pushcfunction(L, luasandbox_base_tostring);
	lua_setglobal(L, "tostring");
	lua_pushcfunction(L, luasandbox_base_pcall);
	lua_setglobal(L, "pcall");
	lua_pushcfunction(L, luasandbox_base_xpcall);
	lua_setglobal(L, "xpcall");
	lua_pushcfunction(L, luasandbox_base_unpack);
	lua_setglobal(L, "unpack");

#if LUA_VERSION_NUM >= 502
	// Lua 5.2 removed setfenv/getfenv in favour of the _ENV upvalue.
	// Reimplement them on top of _ENV so that code which builds an
	// isolation model on stack-level setfenv/getfenv, such as Scribunto,
	// keeps working.
	lua_pushcfunction(L, luasandbox_base_setfenv);
	lua_setglobal(L, "setfenv");
	lua_pushcfunction(L, luasandbox_base_getfenv);
	lua_setglobal(L, "getfenv");
#endif

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

	// Install our own versions of pairs and ipairs to preserve __pairs and
	// __ipairs support across the Lua versions supported by LuaSandbox.
	lua_getglobal(L, "pairs");
	lua_setfield(L, LUA_REGISTRYINDEX, "luasandbox_old_pairs");
	lua_getglobal(L, "ipairs");
	lua_setfield(L, LUA_REGISTRYINDEX, "luasandbox_old_ipairs");
	lua_pushcfunction(L, luasandbox_base_pairs);
	lua_setglobal(L, "pairs");
	lua_pushcfunction(L, luasandbox_base_ipairs);
	lua_setglobal(L, "ipairs");
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
			lua_pushinteger(L, (lua_Integer)(floor(r*u)+1));  /* int between 1 and `u' */
			break;
		}
		case 2: {  /* lower and upper limits */
			int l = luaL_checkint(L, 1);
			int u = luaL_checkint(L, 2);
			luaL_argcheck(L, l<=u, 2, "interval is empty");
			lua_pushinteger(L, (lua_Integer)(floor(r*(u-l+1))+l));  /* int between `l' and `u' */
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

/** {{{ luasandbox_base_unpack
 *
 * Fix an integer overflow vulnerability by applying the fix from Lua 5.2.3
 * https://phabricator.wikimedia.org/T408135
 */
static int luasandbox_base_unpack(lua_State * L) {
	int i, e;
	unsigned int n;
	luaL_checktype(L, 1, LUA_TTABLE);
	i = luaL_optint(L, 2, 1);
	e = luaL_opt(L, luaL_checkint, 3, luaL_getn(L, 1));
	if (i > e) {
		// empty range
		return 0;
	}
	n = (unsigned int)e - (unsigned int)i;  /* number of elements minus 1 */
	if (n > (INT_MAX - 10) || !lua_checkstack(L, ++n)) {
		return luaL_error(L, "too many results to unpack");
	}
	lua_rawgeti(L, 1, i);  /* push arg[i] (avoiding overflow problems) */
	while (i++ < e) {
		// push arg[i + 1...e]
		lua_rawgeti(L, 1, i);
	}
	return n;
}
/* }}} */

#if LUA_VERSION_NUM >= 502
/** {{{ luasandbox_compat_getfunc
 *
 * Equivalent of Lua 5.1's lbaselib.c:getfunc(), which resolves argument 1 to
 * a function: either the function value itself, or the function running at
 * a given stack level (1 = the function that called setfenv/getfenv).
 *
 * Lua 5.1 numbers stack levels so that a tail call is visible as its own
 * level, with no function: it leaves a placeholder "tail" debug frame where
 * the caller used to be, since the real caller was already popped. Lua 5.2+
 * no longer leaves that placeholder; the elided frame is simply gone and
 * lua_getstack skips over it, which would silently shift every level number
 * above a tail call. To keep the same numbering (and the same error for a
 * level that lands on an elided frame) as Lua 5.1, walk the real stack and
 * account for one virtual "tail" level each time ar.istailcall is set.
 */
static void luasandbox_compat_getfunc(lua_State * L, int opt)
{
	if (lua_isfunction(L, 1)) {
		lua_pushvalue(L, 1);
	} else {
		lua_Debug ar;
		int level = opt ? luaL_optint(L, 1, 1) : luaL_checkint(L, 1);
		int real = 0;
		int virt = 0;
		int phantom = 0;
		luaL_argcheck(L, level >= 0, 1, "level must be non-negative");
		while (virt < level) {
			if (phantom) {
				// Step past the virtual "tail" level onto the real frame
				// that follows it.
				real++;
				virt++;
				phantom = 0;
				continue;
			}
			if (!lua_getstack(L, real, &ar)) {
				luaL_argerror(L, 1, "invalid level");
			}
			lua_getinfo(L, "t", &ar);
			if (ar.istailcall) {
				phantom = 1;
				virt++;
			} else {
				real++;
				virt++;
			}
		}
		if (phantom) {
			luaL_error(L, "no function environment for tail call at level %d",
				level);
		}
		if (!lua_getstack(L, real, &ar)) {
			luaL_argerror(L, 1, "invalid level");
		}
		lua_getinfo(L, "f", &ar);
	}
}
/* }}} */

/** {{{ luasandbox_compat_push_env
 *
 * Push the environment of the function at the given stack index: the value
 * of its "_ENV" upvalue, or the sandbox's global table if it has none (e.g.
 * a function which never refers to a global).
 */
static void luasandbox_compat_push_env(lua_State * L, int funcidx)
{
	int i = 1;
	const char * name;
	funcidx = luasandbox_lua_absindex(L, funcidx);
	while ((name = lua_getupvalue(L, funcidx, i)) != NULL) {
		if (strcmp(name, "_ENV") == 0) {
			return;
		}
		lua_pop(L, 1);
		i++;
	}
	luasandbox_pushglobaltable(L);
}
/* }}} */

/** {{{ luasandbox_compat_set_env
 *
 * Set the environment of the function at the given stack index by
 * overwriting its "_ENV" upvalue with the value at validx. Returns 1 on
 * success, or 0 if the function has no "_ENV" upvalue to overwrite.
 */
static int luasandbox_compat_set_env(lua_State * L, int funcidx, int validx)
{
	int i = 1;
	const char * name;
	funcidx = luasandbox_lua_absindex(L, funcidx);
	validx = luasandbox_lua_absindex(L, validx);
	while ((name = lua_getupvalue(L, funcidx, i)) != NULL) {
		lua_pop(L, 1);
		if (strcmp(name, "_ENV") == 0) {
			lua_pushvalue(L, validx);
			lua_setupvalue(L, funcidx, i);
			return 1;
		}
		i++;
	}
	return 0;
}
/* }}} */

/** {{{ luasandbox_base_getfenv
 *
 * Reimplementation of Lua 5.1's getfenv() for Lua 5.2+, on top of the _ENV
 * upvalue. See luasandbox_compat_getfunc() for the stack-level numbering
 * caveat around tail calls.
 */
static int luasandbox_base_getfenv(lua_State * L)
{
	luasandbox_compat_getfunc(L, 1);
	if (lua_iscfunction(L, -1)) {
		// As with Lua 5.1's LUA_GLOBALSINDEX, a C function has no
		// environment of its own; fall back to the sandbox's globals.
		luasandbox_pushglobaltable(L);
	} else {
		luasandbox_compat_push_env(L, -1);
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_base_setfenv
 *
 * Reimplementation of Lua 5.1's setfenv() for Lua 5.2+, on top of the _ENV
 * upvalue. See luasandbox_compat_getfunc() for the stack-level numbering
 * caveat around tail calls.
 *
 * Unlike Lua 5.1, setfenv(0, t) is not supported: Lua 5.1 used it to change
 * the running thread's default environment, but Lua 5.2+ has no equivalent
 * concept to change, only per-function _ENV upvalues.
 */
static int luasandbox_base_setfenv(lua_State * L)
{
	int funcidx;
	luaL_checktype(L, 2, LUA_TTABLE);
	if (lua_isnumber(L, 1) && lua_tonumber(L, 1) == 0) {
		return luaL_error(L,
			LUA_QL("setfenv") " cannot change environment of given object");
	}
	luasandbox_compat_getfunc(L, 0);
	funcidx = lua_gettop(L);
	if (lua_iscfunction(L, funcidx)
		|| !luasandbox_compat_set_env(L, funcidx, 2)
	) {
		return luaL_error(L,
			LUA_QL("setfenv") " cannot change environment of given object");
	}
	lua_pushvalue(L, funcidx);
	return 1;
}
/* }}} */
#endif

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
