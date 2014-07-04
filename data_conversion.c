
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <limits.h>
#include <float.h>
#include <math.h>

#include "php.h"
#include "php_luasandbox.h"
#include "zend_exceptions.h"

#ifdef HHVM
#include <set>
static IMPLEMENT_THREAD_LOCAL(std::set<HashTable*>, protectionSet);
using std::isfinite;
#endif

static inline int luasandbox_is_recursive(HashTable * ht);
static inline void luasandbox_protect_recursion(HashTable * ht);
static inline void luasandbox_unprotect_recursion(HashTable * ht);

static int luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC);
static int luasandbox_free_zval_userdata(lua_State * L);
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht);
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
int luasandbox_push_zval(lua_State * L, zval * z)
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
		case IS_OBJECT: {
			TSRMLS_FETCH();
			zend_class_entry * objce;
			
			objce = Z_OBJCE_P(z);
			if (instanceof_function(objce, luasandboxfunction_ce TSRMLS_CC)) {
				php_luasandboxfunction_obj * func_obj;
				
				func_obj = (php_luasandboxfunction_obj *)zend_object_store_get_object(z TSRMLS_CC);
				
				lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
				lua_rawgeti(L, -1, func_obj->index);
				lua_remove(L, -2);
				break;
			}
		
			if (!luasandbox_push_hashtable(L, Z_OBJPROP_P(z))) {
				return 0;
			}
			break;
		}
		case IS_STRING:
			lua_pushlstring(L, Z_STRVAL_P(z), Z_STRLEN_P(z));
			break;
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
	zval ** zpp = (zval**)lua_touserdata(L, 1);
	php_luasandbox_obj * intern = luasandbox_get_php_obj(L);

	// Don't abort if the request has timed out, we need to be able to clean up
	luasandbox_enter_php_ignore_timeouts(L, intern);

	if (zpp && *zpp) {
		zval_ptr_dtor(zpp);
	}
	*zpp = NULL;
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
	zval ** ud;
	ud = (zval**)lua_newuserdata(L, sizeof(zval*));
	*ud = z;
	Z_ADDREF_P(z);

	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_zval_metatable");
	lua_setmetatable(L, -2);
}
/* }}} */

/** {{{ luasandbox_push_hashtable
 *
 * Helper function for luasandbox_push_zval. Create a new table on the top of 
 * the stack and add the zvals in the HashTable to it. 
 */
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht)
{
	HashPosition p;

	// Recursion requires an arbitrary amount of stack space so we have to 
	// check the stack.
	luaL_checkstack(L, 10, "converting PHP array to Lua");

	lua_newtable(L);
	if (!ht || !zend_hash_num_elements(ht)) {
		return 1;
	}

	if (luasandbox_is_recursive(ht)) {
		TSRMLS_FETCH();
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "recursion detected");
		return 0;
	}
	luasandbox_protect_recursion(ht);
	for (zend_hash_internal_pointer_reset_ex(ht, &p);
			zend_hash_get_current_key_type_ex(ht, &p) != HASH_KEY_NON_EXISTANT;
			zend_hash_move_forward_ex(ht, &p))
	{
		char * key = "";
		uint key_length = 0;
		ulong lkey = 0;
		zval ** value;
		int key_type = zend_hash_get_current_key_ex(ht, &key, &key_length,
				&lkey, 0, &p);
		zend_hash_get_current_data_ex(ht, (void**)&value, &p);
		if (key_type == HASH_KEY_IS_STRING) {
			lua_pushlstring(L, key, key_length - 1);
		} else {
			lua_pushinteger(L, lkey);
		}

		if (!luasandbox_push_zval(L, *value)) {
			// Failed to process that data value
			// Pop the key and the half-constructed table
			lua_pop(L, 2);
			luasandbox_unprotect_recursion(ht);
			return 0;
		}

		lua_settable(L, -3);
	}
	luasandbox_unprotect_recursion(ht);
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
			ZVAL_STRINGL(z, str, length, 1);
			break;
		}
		case LUA_TTABLE: {
			const void * ptr = lua_topointer(L, index);
			void * data = NULL;
			int allocated = 0;
			int success = 1;
			if (recursionGuard) {
				// Check for circular reference (infinite recursion)
				if (zend_hash_find(recursionGuard, (char*)&ptr, sizeof(void*), &data) == SUCCESS) {
					// Found circular reference!
					zval *zex, *ztrace;
					MAKE_STD_ZVAL(zex);
					object_init_ex(zex, luasandboxruntimeerror_ce);

					ALLOC_INIT_ZVAL(ztrace); // IS_NULL if lua_to_zval fails.
					luasandbox_push_structured_trace(L, 1);
					luasandbox_lua_to_zval(ztrace, L, -1, sandbox_zval, NULL TSRMLS_CC);
					zend_update_property(luasandboxruntimeerror_ce, zex, "luaTrace", sizeof("luaTrace")-1, ztrace TSRMLS_CC);
					zval_ptr_dtor(&ztrace);
					lua_pop(L, 1);

					zend_update_property_string(luasandboxruntimeerror_ce, zex,
						"message", sizeof("message")-1, "Cannot pass circular reference to PHP" TSRMLS_CC);
					zend_update_property_long(luasandboxruntimeerror_ce, zex, "code", sizeof("code")-1, -1 TSRMLS_CC);
					zend_throw_exception_object(zex TSRMLS_CC);

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
			zend_hash_update(recursionGuard, (char*)&ptr, sizeof(void*), (void*)"", 1, NULL);

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
			php_luasandbox_obj * sandbox = (php_luasandbox_obj*)
				zend_object_store_get_object(sandbox_zval TSRMLS_CC);

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
			func_obj = (php_luasandboxfunction_obj*)zend_object_store_get_object(z TSRMLS_CC);
			func_obj->index = func_index;
			func_obj->sandbox = sandbox_zval;
			Z_ADDREF_P(sandbox_zval);

			// Balance the stack
			lua_pop(L, 1);
			break;
		}
		case LUA_TUSERDATA:
		case LUA_TTHREAD:
		case LUA_TLIGHTUSERDATA:
			// TODO: provide derived classes for each type
		default: {
			zval *zex, *ztrace;
			char *message;

			spprintf(&message, 0, "Cannot pass %s to PHP", lua_typename(L, lua_type(L, index)));

			MAKE_STD_ZVAL(zex);
			object_init_ex(zex, luasandboxruntimeerror_ce);

			ALLOC_INIT_ZVAL(ztrace); // IS_NULL if lua_to_zval fails.
			luasandbox_push_structured_trace(L, 1);
			luasandbox_lua_to_zval(ztrace, L, -1, sandbox_zval, NULL TSRMLS_CC);
			zend_update_property(luasandboxruntimeerror_ce, zex, "luaTrace", sizeof("luaTrace")-1, ztrace TSRMLS_CC);
			zval_ptr_dtor(&ztrace);
			lua_pop(L, 1);

			zend_update_property_string(luasandboxruntimeerror_ce, zex,
				"message", sizeof("message")-1, message TSRMLS_CC);
			zend_update_property_long(luasandboxruntimeerror_ce, zex, "code", sizeof("code")-1, -1 TSRMLS_CC);
			zend_throw_exception_object(zex TSRMLS_CC);

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
	const char * str;
	size_t length;
	zval *value;
	lua_Number n;
	int top = lua_gettop(L);

	// Recursion requires an arbitrary amount of stack space so we have to
	// check the stack.
	luaL_checkstack(L, 15, "converting Lua table to PHP");

	// Normalise the input index so that we can push without invalidating it.
	if (index < 0) {
		index += top + 1;
	}

	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		ALLOC_INIT_ZVAL(value); // ensure is inited even if lua_to_zval bails.
		if (!luasandbox_lua_to_zval(value, L, -1, sandbox_zval, recursionGuard TSRMLS_CC)) {
			// Conversion failed, fix stack and bail
			zval_ptr_dtor(&value);
			lua_settop(L, top);
			return 0;
		}

		if (lua_type(L, -2) == LUA_TNUMBER) {
			n = lua_tonumber(L, -2);
			if (isfinite(n) && n == floor(n)) {
				// Integer key
				zend_hash_index_update(ht, n, (void*)&value, sizeof(zval*), NULL);
				lua_settop(L, top + 1);
				continue;
			}
		}

		// Make a copy of the key so that we can call lua_tolstring() which is destructive
		lua_pushvalue(L, -2);
		str = lua_tolstring(L, -1, &length);
		if ( str == NULL ) {
			// Only strings and integers may be used as keys
			zval *zex, *ztrace;
			char *message;

			spprintf(&message, 0, "Cannot use %s as an array key when passing data from Lua to PHP",
				lua_typename(L, lua_type(L, -3))
			);

			MAKE_STD_ZVAL(zex);
			object_init_ex(zex, luasandboxruntimeerror_ce);

			MAKE_STD_ZVAL(ztrace);
			luasandbox_push_structured_trace(L, 1);
			luasandbox_lua_to_zval(ztrace, L, -1, sandbox_zval, NULL TSRMLS_CC);
			zend_update_property(luasandboxruntimeerror_ce, zex, "luaTrace", sizeof("luaTrace")-1, ztrace TSRMLS_CC);
			zval_ptr_dtor(&ztrace);
			lua_pop(L, 1);

			zend_update_property_string(luasandboxruntimeerror_ce, zex,
				"message", sizeof("message")-1, message TSRMLS_CC);
			zend_update_property_long(luasandboxruntimeerror_ce, zex, "code", sizeof("code")-1, -1 TSRMLS_CC);
			zend_throw_exception_object(zex TSRMLS_CC);

			efree(message);

			lua_settop(L, top);
			return 0;
		}
		zend_hash_update(ht, str, length + 1, (void*)&value, sizeof(zval*), NULL);

		// Pop temporary values off the stack
		lua_settop(L, top + 1);
	}
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
 */
int luasandbox_is_fatal(lua_State * L, int index)
{
	return luasandbox_has_error_marker(L, index, &luasandbox_fatal_error_marker);
}
/* }}} */

/** {{{ luasandbox_is_trace_error
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

/** {{{ luasandbox_is_recursive
 * Check if the recursion flag is set on this hashtable.
 *
 * Under Zend, this is implemented using nApplyCount, following the example
 * of HASH_PROTECT_RECURSION() in zend_hash.c. Under HHVM, this is implemented
 * with a std::set of pointer values, since the HashTable object itself has
 * no space for storing a recursion level.
 */
static inline int luasandbox_is_recursive(HashTable * ht) {
#ifdef HHVM
	return (int)(protectionSet.get()->count(ht));
#else
	return ht->nApplyCount;
#endif
}

/** {{{ luasandbox_protect_recursion
 *
 * Increment the recursion level
 */
static inline void luasandbox_protect_recursion(HashTable * ht) {
#ifdef HHVM
	protectionSet.get()->insert(ht);
#else
	ht->nApplyCount++;
#endif
}

/** {{{ luasandbox_protect_recursion
 *
 * Decrement the recursion level
 */
static inline void luasandbox_unprotect_recursion(HashTable * ht) {
#ifdef HHVM
	protectionSet.get()->erase(ht);
#else
	ht->nApplyCount--;
#endif
}

