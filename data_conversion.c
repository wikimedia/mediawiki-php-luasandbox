
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
#include "luasandbox_unicode.h"

static void luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC);
static int luasandbox_free_zval_userdata(lua_State * L);
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht);

extern zend_class_entry *luasandboxfunction_ce;
extern zend_class_entry *luasandboxplaceholder_ce;

/**
 * An int, the address of which is used as a fatal error marker. The value is 
 * not used.
 */
int luasandbox_fatal_error_marker = 0;


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
		case IS_CONSTANT:
		case IS_CONSTANT_ARRAY:
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
	luasandbox_enter_php(L, intern);
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
 * Convert a lua value to a zval.
 *
 * If a value is encountered that can't be converted to a zval, a LuaPlaceholder
 * object is returned instead.
 *
 * @param z A pointer to the destination zval
 * @param L The lua state
 * @param index The stack index to the input value
 * @param sandbox_zval A zval poiting to a valid LuaSandbox object which will be
 *     used for the parent object of any LuaSandboxFunction objects created.
 * @param recursionGuard A hashtable for keeping track of tables that have been 
 *     processed, to allow infinite recursion to be avoided. External callers 
 *     should set this to NULL.
 */
void luasandbox_lua_to_zval(zval * z, lua_State * L, int index, 
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
			zend_hash_update(recursionGuard, (char*)&ptr, sizeof(void*), "", 1, NULL);

			// Process the array
			array_init(z);
			luasandbox_lua_to_array(Z_ARRVAL_P(z), L, index, sandbox_zval, recursionGuard TSRMLS_CC);

			if (allocated) {
				zend_hash_destroy(recursionGuard);
				FREE_HASHTABLE(recursionGuard);
			}
			break;
		}
		case LUA_TFUNCTION: {
			int func_index;
			php_luasandboxfunction_obj * func_obj;
			php_luasandbox_obj * sandbox = (php_luasandbox_obj*)
				zend_object_store_get_object(sandbox_zval);

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
			func_obj = (php_luasandboxfunction_obj*)zend_object_store_get_object(z);
			func_obj->index = func_index;
			func_obj->sandbox = sandbox_zval;
			Z_ADDREF_P(sandbox_zval);

			// Balance the stack
			lua_pop(L, 1);
			break;
		}
		case LUA_TUSERDATA:
			if(luasandbox_isustr(L, index)) {
				const uint8_t *str;
				size_t length;
				str = luasandbox_getustr(L, index, &length);
				ZVAL_STRINGL(z, str, length, 1);
				break;
			}
		case LUA_TTHREAD:
		case LUA_TLIGHTUSERDATA:
		default:
			// TODO: provide derived classes for each type
			object_init_ex(z, luasandboxplaceholder_ce);
	}
}
/* }}} */

/** {{{ luasandbox_lua_to_array
 *
 * Append the elements of the table in the specified index to the given HashTable.
 */
static void luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	zval * sandbox_zval, HashTable * recursionGuard TSRMLS_DC)
{
	const char * str;
	size_t length;
	zval *value;
	lua_Number n;
	int top = lua_gettop(L);

	// Normalise the input index so that we can push without invalidating it.
	if (index < 0) {
		index += top + 1;
	}

	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		MAKE_STD_ZVAL(value);
		luasandbox_lua_to_zval(value, L, -1, sandbox_zval, recursionGuard TSRMLS_CC);
		
		if (lua_type(L, -2) == LUA_TNUMBER) {
			n = lua_tonumber(L, -2);
			if (n == floor(n)) {
				// Integer key
				zend_hash_index_update(ht, n, (void*)&value, sizeof(zval*), NULL);
				lua_settop(L, top + 1);
				continue;
			}
		}

		// Make a copy of the key so that we can call lua_tolstring() which is destructive
		lua_pushvalue(L, -2);
		str = lua_tolstring(L, -1, &length);
		zend_hash_update(ht, str, length + 1, (void*)&value, sizeof(zval*), NULL);

		// Pop temporary values off the stack
		lua_settop(L, top + 1);
	}
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
	void * ud;
	int haveIndex2 = 0;

	if (!lua_istable(L, index)) {
		return 0;
	}

	lua_rawgeti(L, index, 1);
	ud = lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (ud != &luasandbox_fatal_error_marker) {
		return 0;
	}

	lua_rawgeti(L, index, 2);
	haveIndex2 = !lua_isnil(L, -1);
	lua_pop(L, 1);
	return haveIndex2;
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
	if (luasandbox_is_fatal(L, index)) {
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

