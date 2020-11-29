
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <inttypes.h>

#include "php.h"
#include "php_luasandbox.h"
#include "zend_exceptions.h"

#ifdef HHVM
using std::isfinite;
#endif

static void luasandbox_throw_runtimeerror(lua_State * L, zval * sandbox_zval, const char *message TSRMLS_DC);

static inline int luasandbox_protect_recursion(zval * z, HashTable ** recursionGuard, int * allocated);
static inline void luasandbox_unprotect_recursion(zval * z, HashTable * recursionGuard, int allocated);

static int luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC);
static int luasandbox_lua_pair_to_array(HashTable *ht, lua_State *L,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC);
static int luasandbox_free_zval_userdata(lua_State * L);
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht, HashTable * recursionGuard);
static int luasandbox_has_error_marker(lua_State * L, int index, void * marker);

extern zend_class_entry *luasandboxfunction_ce;
extern zend_class_entry *luasandboxruntimeerror_ce;

/**
 * An int, the address of which is used as a fatal error marker. The value is
 * not used.
 */
int luasandbox_fatal_error_marker = 0;

/**
 * Same as luasandbox_fatal_error_marker but for trace errors
 */
int luasandbox_trace_error_marker = 0;

/** {{{ luasandbox_data_conversion_init
 *
 * Set up a lua_State so that this module can work with it.
 */
void luasandbox_data_conversion_init(lua_State * L)
{
	// Create the metatable for zval destruction
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, luasandbox_free_zval_userdata);
	lua_setfield(L, -2, "__gc");
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_zval_metatable");
}
/* }}} */

/** {{{ luasandbox_push_zval
 *
 * Convert a zval to an appropriate Lua type and push the resulting value on to
 * the stack.
 */
int luasandbox_push_zval(lua_State * L, zval * z, HashTable * recursionGuard)
{
	switch (Z_TYPE_P(z)) {
#ifdef IS_UNDEF
		case IS_UNDEF: // Close enough to IS_NULL
#endif
		case IS_NULL:
			lua_pushnil(L);
			break;
		case IS_LONG:
			lua_pushinteger(L, Z_LVAL_P(z));
			break;
		case IS_DOUBLE:
			lua_pushnumber(L, Z_DVAL_P(z));
			break;
#ifdef IS_BOOL
		case IS_BOOL:
			lua_pushboolean(L, Z_BVAL_P(z));
			break;
#endif
#ifdef IS_TRUE
		case IS_TRUE:
			lua_pushboolean(L, 1);
			break;
		case IS_FALSE:
			lua_pushboolean(L, 0);
			break;
#endif
		case IS_ARRAY: {
			int ret, allocated = 0;
			if (!luasandbox_protect_recursion(z, &recursionGuard, &allocated)) {
				return 0;
			}
			ret = luasandbox_push_hashtable(L, Z_ARRVAL_P(z), recursionGuard);
			luasandbox_unprotect_recursion(z, recursionGuard, allocated);
			return ret;
		}
		case IS_OBJECT: {
			TSRMLS_FETCH();
			zend_class_entry * objce;

			objce = Z_OBJCE_P(z);
			if (instanceof_function(objce, luasandboxfunction_ce TSRMLS_CC)) {
				php_luasandboxfunction_obj * func_obj;

				func_obj = GET_LUASANDBOXFUNCTION_OBJ(z);

				lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
				lua_rawgeti(L, -1, func_obj->index);
				lua_remove(L, -2);
				break;
			}

			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Unable to convert object of type %s",
				ZSTR_VAL(objce->name)
			);

			return 0;
		}
		case IS_STRING:
			lua_pushlstring(L, Z_STRVAL_P(z), Z_STRLEN_P(z));
			break;
#ifdef IS_REFERENCE
		case IS_REFERENCE: {
			int ret, allocated = 0;
			if (!luasandbox_protect_recursion(z, &recursionGuard, &allocated)) {
				return 0;
			}
			ret = luasandbox_push_zval(L, Z_REFVAL_P(z), recursionGuard);
			luasandbox_unprotect_recursion(z, recursionGuard, allocated);
			return ret;
		}
#endif

		case IS_RESOURCE:
		default:
			return 0;
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_free_zval_userdata
 *
 * Free a zval given to Lua by luasandbox_push_zval_userdata.
 */
static int luasandbox_free_zval_userdata(lua_State * L)
{
	zval * ud = (zval*)lua_touserdata(L, 1);
	php_luasandbox_obj * intern = luasandbox_get_php_obj(L);

	// Don't abort if the request has timed out, we need to be able to clean up
	luasandbox_enter_php_ignore_timeouts(L, intern);

	if (ud && !Z_ISUNDEF_P(ud)) {
		zval_ptr_dtor(ud);
		ZVAL_UNDEF(ud);
	}

	luasandbox_leave_php(L, intern);
	return 0;
}
/* }}} */

/** {{{ luasandbox_push_zval_userdata
 *
 * Push a full userdata on to the stack, which stores a zval* in its block.
 * Increment its reference count and set its metatable so that it will be freed
 * at the appropriate time.
 */
void luasandbox_push_zval_userdata(lua_State * L, zval * z)
{
	zval * ud = (zval*)lua_newuserdata(L, sizeof(zval));
	ZVAL_COPY(ud, z);

	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_zval_metatable");
	lua_setmetatable(L, -2);
}
/* }}} */

/** {{{ luasandbox_push_hashtable
 *
 * Helper function for luasandbox_push_zval. Create a new table on the top of
 * the stack and add the zvals in the HashTable to it.
 */
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht, HashTable * recursionGuard)
{
#if SIZEOF_LONG > 4
	char buffer[MAX_LENGTH_OF_LONG + 1];
#endif

	// Recursion requires an arbitrary amount of stack space so we have to
	// check the stack.
	luaL_checkstack(L, 10, "converting PHP array to Lua");

	lua_newtable(L);
	if (!ht || !zend_hash_num_elements(ht)) {
		return 1;
	}

	ulong lkey;
	zend_string *key;
	zval *value;
	ZEND_HASH_FOREACH_KEY_VAL(ht, lkey, key, value)
	{
		// Lua doesn't represent most integers with absolute value over 2**53,
		// so stringify them.
#if SIZEOF_LONG > 4
		if (!key &&
				((int64_t)lkey > INT64_C(9007199254740992) || (int64_t)lkey < INT64_C(-9007199254740992))
		) {
			size_t len = snprintf(buffer, sizeof(buffer), "%" PRId64, (int64_t)lkey);
			lua_pushlstring(L, buffer, len);
		} else
#endif
		if (key) {
			lua_pushlstring(L, ZSTR_VAL(key), ZSTR_LEN(key));
		} else {
			lua_pushinteger(L, lkey);
		}

		if (!luasandbox_push_zval(L, value, recursionGuard)) {
			// Failed to process that data value
			// Pop the key and the half-constructed table
			lua_pop(L, 2);
			return 0;
		}

		lua_settable(L, -3);
	} ZEND_HASH_FOREACH_END();

	return 1;
}
/* }}} */

/** {{{ luasandbox_lua_to_zval
 *
 * Convert a lua value to a zval.
 *
 * If a value is encountered that can't be converted to a zval, an exception is
 * thrown.
 *
 * @param z A pointer to the destination zval
 * @param L The lua state
 * @param index The stack index to the input value
 * @param sandbox_zval A zval poiting to a valid LuaSandbox object which will be
 *     used for the parent object of any LuaSandboxFunction objects created.
 * @param recursionGuard A hashtable for keeping track of tables that have been
 *     processed, to allow infinite recursion to be avoided. External callers
 *     should set this to NULL.
 * @return int 0 (and a PHP exception) on failure
 */
int luasandbox_lua_to_zval(zval * z, lua_State * L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC)
{
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
			ZVAL_STRINGL(z, str, length);
			break;
		}
		case LUA_TTABLE: {
			const void * ptr = lua_topointer(L, index);
			int allocated = 0;
			int success = 1;
			if (recursionGuard) {
				// Check for circular reference (infinite recursion)
				if (zend_hash_str_exists(recursionGuard, (char*)&ptr, sizeof(void*))) {
					// Found circular reference!
					luasandbox_throw_runtimeerror(L, sandbox_zval, "Cannot pass circular reference to PHP" TSRMLS_CC);

					ZVAL_NULL(z); // Need to set something to prevent a segfault
					return 0;
				}
			} else {
				ALLOC_HASHTABLE(recursionGuard);
				zend_hash_init(recursionGuard, 1, NULL, NULL, 0);
				allocated = 1;
			}

			// Add the current table to the recursion guard hashtable
			// Use the pointer as the key, zero-length data
			zval zv;
			ZVAL_TRUE(&zv);
			zend_hash_str_update(recursionGuard, (char*)&ptr, sizeof(void*), &zv);

			// Process the array
			array_init(z);
			success = luasandbox_lua_to_array(Z_ARRVAL_P(z), L, index, sandbox_zval, recursionGuard TSRMLS_CC);

			if (allocated) {
				zend_hash_destroy(recursionGuard);
				FREE_HASHTABLE(recursionGuard);
			}

			if (!success) {
				// free the array created by array_init() above.
				zval_dtor(z);
				ZVAL_NULL(z);
				return 0;
			}
			break;
		}
		case LUA_TFUNCTION: {
			int func_index;
			php_luasandboxfunction_obj * func_obj;
			php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(sandbox_zval);

			// Normalise the input index so that we can push without invalidating it.
			if (index < 0) {
				index += lua_gettop(L) + 1;
			}

			// Get the chunks table
			lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");

			// Get the next free index
			if (sandbox->function_index >= INT_MAX) {
				ZVAL_NULL(z);
				lua_pop(L, 1);
				break;
			}
			func_index = ++(sandbox->function_index);

			// Store it in the chunks table
			lua_pushvalue(L, index);
			lua_rawseti(L, -2, func_index);

			// Create a LuaSandboxFunction object to hold a reference to the function
			object_init_ex(z, luasandboxfunction_ce);
			func_obj = GET_LUASANDBOXFUNCTION_OBJ(z);
			func_obj->index = func_index;
			ZVAL_COPY(&func_obj->sandbox, sandbox_zval);

			// Balance the stack
			lua_pop(L, 1);
			break;
		}
		case LUA_TUSERDATA:
		case LUA_TTHREAD:
		case LUA_TLIGHTUSERDATA:
			// TODO: provide derived classes for each type
		default: {
			char *message;
			spprintf(&message, 0, "Cannot pass %s to PHP", lua_typename(L, lua_type(L, index)));
			luasandbox_throw_runtimeerror(L, sandbox_zval, message TSRMLS_CC);
			efree(message);

			ZVAL_NULL(z); // Need to set something to prevent a segfault
			return 0;
		}
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_lua_to_array
 *
 * Append the elements of the table in the specified index to the given HashTable.
 */
static int luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC)
{
	php_luasandbox_obj * sandbox;
	int top = lua_gettop(L);

	// Recursion requires an arbitrary amount of stack space so we have to
	// check the stack.
	luaL_checkstack(L, 15, "converting Lua table to PHP");

	// Normalise the input index so that we can push without invalidating it.
	if (index < 0) {
		index += top + 1;
	}

	// If the input table has a __pairs function, we need to use that instead
	// of lua_next.
	if (luaL_getmetafield(L, index, "__pairs")) {
		sandbox = luasandbox_get_php_obj(L);

		// Put the error handler function onto the stack
		lua_pushcfunction(L, luasandbox_attach_trace);
		lua_insert(L, top + 1);

		// Call __pairs
		lua_pushvalue(L, index);
		if (!luasandbox_call_lua(sandbox, sandbox_zval, 1, 3, top + 1 TSRMLS_CC)) {
			// Failed to call __pairs. Cleanup stack and return failure.
			lua_settop(L, top);
			return 0;
		}
		while (1) {
			// We need to copy static-data and func so we can reuse them for
			// each iteration.
			lua_pushvalue(L, -3);
			lua_insert(L, -2);
			lua_pushvalue(L, -3);
			lua_insert(L, -2);

			// Call the custom 'next' function from __pairs
			if (!luasandbox_call_lua(sandbox, sandbox_zval, 2, 2, top + 1 TSRMLS_CC)) {
				// Failed. Cleanup stack and return failure.
				lua_settop(L, top);
				return 0;
			}

			if (lua_isnil(L, -2)) {
				// Nil key == end. Cleanup stack and exit loop.
				lua_settop(L, top);
				break;
			}
			if (!luasandbox_lua_pair_to_array(ht, L, sandbox_zval, recursionGuard TSRMLS_CC)) {
				// Failed to convert value. Cleanup stack and return failure.
				lua_settop(L, top);
				return 0;
			}
		}
	} else {
		// No __pairs, we can use lua_next
		lua_pushnil(L);
		while (lua_next(L, index) != 0) {
			if (!luasandbox_lua_pair_to_array(ht, L, sandbox_zval, recursionGuard TSRMLS_CC)) {
				// Failed to convert value. Cleanup stack and return failure.
				lua_settop(L, top);
				return 0;
			}
		}
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_lua_pair_to_array
 *
 * Take the lua key-value pair at the top of the Lua stack and add it to the given HashTable.
 * On success the value is popped, but the key remains on the stack.
 */
static int luasandbox_lua_pair_to_array(HashTable *ht, lua_State *L,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC)
{
	const char * str;
	size_t length;
	lua_Number n;
	zend_ulong zn;

	zval value, *valp = &value;
	ZVAL_NULL(&value);

	// Convert value, then remove it
	if (!luasandbox_lua_to_zval(valp, L, -1, sandbox_zval, recursionGuard TSRMLS_CC)) {
		zval_ptr_dtor(&value);
		return 0;
	}
	lua_pop(L, 1);

	// Convert key, but leave it there
	if (lua_type(L, -1) == LUA_TNUMBER) {
		n = lua_tonumber(L, -1);
		if (isfinite(n) && n == floor(n) && n >= LONG_MIN && n <= LONG_MAX) {
			zn = (long)n;
			goto add_int_key;
		}
	}

	// Make a copy of the key so that we can call lua_tolstring() which is destructive
	lua_pushvalue(L, -1);
	str = lua_tolstring(L, -1, &length);
	if ( str == NULL ) {
		// Only strings and integers may be used as keys
		char *message;
		spprintf(&message, 0, "Cannot use %s as an array key when passing data from Lua to PHP",
			lua_typename(L, lua_type(L, -2))
		);
		zval_ptr_dtor(&value);
		luasandbox_throw_runtimeerror(L, sandbox_zval, message TSRMLS_CC);
		efree(message);
		return 0;
	}
	lua_pop(L, 1);

	// See if the string is convertable to a number
	if (ZEND_HANDLE_NUMERIC_STR(str, length, zn)) {
		goto add_int_key;
	}

	// Nope, use it as a string
	if (zend_hash_str_exists(ht, str, length)) {
		// Collision, probably the key is an integer-like string
		char *message;
		spprintf(&message, 0, "Collision for array key %s when passing data from Lua to PHP", str );
		zval_ptr_dtor(&value);
		luasandbox_throw_runtimeerror(L, sandbox_zval, message TSRMLS_CC);
		efree(message);
		return 0;
	}
	zend_hash_str_update(ht, str, length, valp);

	return 1;

add_int_key:
	if (zend_hash_index_exists(ht, zn)) {
		// Collision, probably with a integer-like string
		char *message;
		spprintf(&message, 0, "Collision for array key %" PRId64 " when passing data from Lua to PHP",
			(int64_t)zn
		);
		zval_ptr_dtor(&value);
		luasandbox_throw_runtimeerror(L, sandbox_zval, message TSRMLS_CC);
		efree(message);
		return 0;
	}
	zend_hash_index_update(ht, zn, valp);

	return 1;
}
/* }}} */

/** {{{ luasandbox_wrap_fatal
 *
 * Pop a value off the top of the stack, and push a fatal error wrapper
 * containing the value.
 */
void luasandbox_wrap_fatal(lua_State * L)
{
	// Create the table and put the marker in it as element 1
	lua_createtable(L, 0, 2);
	lua_pushlightuserdata(L, &luasandbox_fatal_error_marker);
	lua_rawseti(L, -2, 1);

	// Swap the table with the input value, so that the value is on the top,
	// then put the value in the table as element 2
	lua_insert(L, -2);
	lua_rawseti(L, -2, 2);
}
/* }}} */

/** {{{ luasandbox_is_fatal
 *
 * Check if the value at the given stack index is a fatal error wrapper
 * created by luasandbox_wrap_fatal(). Return 1 if it is, 0 otherwise.
 *
 * This function cannot raise Lua errors.
 */
int luasandbox_is_fatal(lua_State * L, int index)
{
	return luasandbox_has_error_marker(L, index, &luasandbox_fatal_error_marker);
}
/* }}} */

/** {{{ luasandbox_is_trace_error
 *
 * Check if the value at the given stack index is an error wrapper created by
 * luasandbox_attach_trace(). Return 1 if it is, 0 otherwise.
 *
 * This function cannot raise Lua errors.
 */
int luasandbox_is_trace_error(lua_State * L, int index)
{
	return luasandbox_has_error_marker(L, index, &luasandbox_trace_error_marker);
}
/* }}} */

/** {{{
 *
 * Check if the error at the given stack index has a given marker userdata
 */
static int luasandbox_has_error_marker(lua_State * L, int index, void * marker)
{
	void * ud;
	if (!lua_istable(L, index)) {
		return 0;
	}
	lua_rawgeti(L, index, 1);
	ud = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ud == marker;
}
/* }}} */

/** {{{
 *
 * If the value at the given stack index is a fatal error wrapper, convert
 * the error object it wraps to a string. If the value is anything else,
 * convert it directly to a string. If the error object is not convertible
 * to a string, return "unknown error".
 *
 * This calls lua_tolstring() and will corrupt the value on the stack as
 * described in that function's documentation. The string is valid until the
 * Lua value is destroyed.
 *
 * This function can raise Lua memory errors, but no other Lua errors.
 */
const char * luasandbox_error_to_string(lua_State * L, int index)
{
	const char * s;
	if (index < 0) {
		index += lua_gettop(L) + 1;
	}
	if (luasandbox_is_fatal(L, index) || luasandbox_is_trace_error(L, index)) {
		lua_rawgeti(L, index, 2);
		s = lua_tostring(L, -1);
		lua_pop(L, 1);
	} else {
		s = lua_tostring(L, index);
	}
	if (!s) {
		return "unknown error";
	} else {
		return s;
	}
}
/* }}} */

/** {{{ luasandbox_attach_trace
 *
 * Error callback function for lua_pcall(): wrap the error value in a table that
 * includes backtrace information.
 */
int luasandbox_attach_trace(lua_State * L)
{
	if (luasandbox_is_fatal(L, 1)) {
		// Pass fatals through unaltered
		return 1;
	}

	// Create the table and put the marker in it as element 1
	lua_createtable(L, 0, 3);
	lua_pushlightuserdata(L, &luasandbox_trace_error_marker);
	lua_rawseti(L, -2, 1);

	// Swap the table with the input value, so that the value is on the top,
	// then put the value in the table as element 2
	lua_insert(L, -2);
	lua_rawseti(L, -2, 2);

	// Put the backtrace in element 3
	luasandbox_push_structured_trace(L, 1);
	lua_rawseti(L, -2, 3);

	return 1;
}
/* }}} */

/** {{{ luasandbox_push_structured_trace
 *
 * Make a table representing the current backtrace and push it to the stack.
 * "level" is the call stack level to start at, 1 for the current function.
 */
void luasandbox_push_structured_trace(lua_State * L, int level)
{
	lua_Debug ar;
	lua_newtable(L);
	int i;

	for (i = level; lua_getstack(L, i, &ar); i++) {
		lua_getinfo(L, "nSl", &ar);
		lua_createtable(L, 0, 8);
		lua_pushstring(L, ar.short_src);
		lua_setfield(L, -2, "short_src");
		lua_pushstring(L, ar.what);
		lua_setfield(L, -2, "what");
		lua_pushnumber(L, ar.currentline);
		lua_setfield(L, -2, "currentline");
		lua_pushstring(L, ar.name);
		lua_setfield(L, -2, "name");
		lua_pushstring(L, ar.namewhat);
		lua_setfield(L, -2, "namewhat");
		lua_pushnumber(L, ar.linedefined);
		lua_setfield(L, -2, "linedefined");
		lua_rawseti(L, -2, i - level + 1);
	}
}
/* }}} */

/** {{{ luasandbox_throw_runtimeerror
 *
 * Create and throw a luasandboxruntimeerror
 */
void luasandbox_throw_runtimeerror(lua_State * L, zval * sandbox_zval, const char *message TSRMLS_DC)
{
	zval *zex, *ztrace;

	zval zvex, zvtrace;
	zex = &zvex;
	ztrace = &zvtrace;
	ZVAL_NULL(ztrace); // IS_NULL if lua_to_zval fails.

	object_init_ex(zex, luasandboxruntimeerror_ce);

	luasandbox_push_structured_trace(L, 1);
	luasandbox_lua_to_zval(ztrace, L, -1, sandbox_zval, NULL TSRMLS_CC);
	zend_update_property(luasandboxruntimeerror_ce, zex, "luaTrace", sizeof("luaTrace")-1, ztrace TSRMLS_CC);
	zval_ptr_dtor(&zvtrace);

	lua_pop(L, 1);

	zend_update_property_string(luasandboxruntimeerror_ce, zex,
		"message", sizeof("message")-1, message TSRMLS_CC);
	zend_update_property_long(luasandboxruntimeerror_ce, zex, "code", sizeof("code")-1, -1 TSRMLS_CC);
	zend_throw_exception_object(zex TSRMLS_CC);
}
/* }}} */

/** {{{ luasandbox_protect_recursion
 *
 * Check that the zval isn't already in recursionGuard, and if so add it. May
 * allocate recursionGuard if necessary.
 *
 * Returns 1 if recursion is not detected, 0 if it was.
 */
static inline int luasandbox_protect_recursion(zval * z, HashTable ** recursionGuard, int * allocated) {
	if (!*recursionGuard) {
		*allocated = 1;
		ALLOC_HASHTABLE(*recursionGuard);
		zend_hash_init(*recursionGuard, 1, NULL, NULL, 0);
	} else if (zend_hash_str_exists(*recursionGuard, (char*)&z, sizeof(void*))) {
		TSRMLS_FETCH();
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot pass circular reference to Lua");
		return 0;
	}

	zval zv;
	ZVAL_TRUE(&zv);
	zend_hash_str_update(*recursionGuard, (char*)&z, sizeof(void*), &zv);

	return 1;
}
/* }}} */

/** {{{ luasandbox_unprotect_recursion
 *
 * Undoes what luasandbox_protect_recursion did.
 */
static inline void luasandbox_unprotect_recursion(zval * z, HashTable * recursionGuard, int allocated) {
	if (allocated) {
		zend_hash_destroy(recursionGuard);
		FREE_HASHTABLE(recursionGuard);
	} else {
		zend_hash_str_del(recursionGuard, (char*)&z, sizeof(void*));
	}
}
/* }}} */
