
/*
 * To do:
 *   * Move sandbox.scripts to the registry, and don't provide access to it to
 *     user scripts, to avoid the potential for a panic in 
 *     LuaSandbox::loadString() if sandbox.scripts is overwritten.
 *
 *   * Remove LuaSandbox::callScript(). Instead, have LuaSandbox::loadString() 
 *     return a LuaSandboxFunction object with a call() method. Store the 
 *     script functions in a table indexed by integer, instead of using the
 *     chunk name, so that nothing strange will happen if there are duplicate
 *     chunk names. Delete the function from the Lua state when the object is
 *     destroyed.
 *
 *   * Provide a LuaSandbox::doString() as a convenience wrapper for 
 *     LuaSandbox::loadString()->call(), analogous to luaL_dostring().
 *
 *   * Add PHP methods for registering callback functions, both individually 
 *     and in bulk (a library creation function).
 *
 *   * Provide a wrapper around the base library, which allows only a whitelist
 *     of functions, not including e.g. loadfile(). This is slightly tricky 
 *     because the functions are not exported from the shared library. It's 
 *     necessary to call luaopen_base() on a separate lua_State, and then to 
 *     copy each function pointer to the destination lua_State using 
 *     lua_tocfunction().
 *
 *   * Add CPU time limits.
 *
 *   Doing a longjmp() from a signal handler destroys anything that the 
 *   call stack may have been modifying at the time. Allowing continued 
 *   access to such state will allow security vulnerabilities (SIG32-C). 
 *
 *   So I propose having two timeouts. When the first expires, a debug hook 
 *   is set which calls lua_error(), and a flag is set prohibiting dispatch
 *   of any PHP callback. When the second expires, emergency action is 
 *   taken. If a PHP callback has been dispatched and we are waiting for it
 *   to return, zend_error() will need to be called with E_ERROR, to safely
 *   destroy the PHP state. This mirrors the behaviour of normal Zend timeouts. 
 *   Otherwise, lua_error() should be called, to return control to the 
 *   lua_pcall() caller, which should then destroy the lua state.
 *
 *   * Add LuaSandbox::getMemoryUsage().
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <math.h>
#include <limits.h>
#include <float.h>

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_luasandbox.h"
#include "zend_exceptions.h"

static zend_object_value luasandbox_new(zend_class_entry *ce TSRMLS_DC);
static void luasandbox_free_storage(void *object TSRMLS_DC);
static void *luasandbox_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
static int luasandbox_panic(lua_State * L);
static lua_State * luasandbox_newstate();
static lua_State * getLuaState(zval * this_ptr);
static int luasandbox_find_field(lua_State * L, int index, 
	char * spec, int specLength);
static void luasandbox_call_helper(INTERNAL_FUNCTION_PARAMETERS, int index);
static int luasandbox_push_zval(lua_State * L, zval * z);
static zval * luasandbox_lua_to_zval(lua_State * L, int index, HashTable * recursionGuard);
static void luasandbox_lua_to_array(HashTable *ht, lua_State *L, int index,
	HashTable * recursionGuard);
static php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L);
static int luasandbox_method_getMemoryUsage(lua_State * L);
static void luasandbox_handle_error(lua_State * L, int status);
static int luasandbox_push_hashtable(lua_State * L, HashTable * ht);

zend_class_entry *luasandbox_ce;
zend_class_entry *luasandboxerror_ce;
zend_class_entry *luasandboxplaceholder_ce;

/** {{{ arginfo */
ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_loadString, 0)
	ZEND_ARG_INFO(0, code)
	ZEND_ARG_INFO(0, chunkName)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setMemoryLimit, 0)
	ZEND_ARG_INFO(0, limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_callFunction, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, ...)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_callScript, 0)
	ZEND_ARG_INFO(0, name)
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
	PHP_ME(LuaSandbox, callFunction, arginfo_luasandbox_callFunction, 0)
	PHP_ME(LuaSandbox, callScript, arginfo_luasandbox_callScript, 0)
	{NULL, NULL, NULL}
};

const zend_function_entry luasandbox_empty_methods[] = {
	{NULL, NULL, NULL}
};

/** sandbox method names and their C functions */
static const luaL_Reg luasandbox_lua_methods[] = {
	{"getMemoryUsage", luasandbox_method_getMemoryUsage},
	{NULL, NULL}
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
	NULL, /* RSHUTDOWN */
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

	// Create the "sandbox" global
	lua_newtable(L);
	// Add its methods
	luaL_register(L, NULL, luasandbox_lua_methods);

	// Add its properties
	lua_newtable(L);
	lua_setfield(L, -2, "scripts");

	// Move the new table from the stack to the sandbox global
	lua_setglobal(L, "sandbox");
	
	// Register a pointer to the PHP object so that C functions can find it
	lua_pushlightuserdata(L, (void*)intern);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");
	
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

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(luasandbox)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "luasandbox support", "enabled");
	php_info_print_table_end();
}
/* }}} */

/** {{{ luasandbox_alloc */
static void *luasandbox_alloc(void *ud, void *ptr, size_t osize, size_t nsize) 
{
	// Update memory usage accounting
	php_luasandbox_obj * obj = (php_luasandbox_obj*)ud;
	if (obj->memory_usage + nsize < obj->memory_usage) {
		// Overflow
		// No PHP error because it's plausible that untrusted scripts could do this.
		return NULL;
	}

	obj->memory_usage += nsize - osize;

	if (nsize == 0) {
		if (ptr) {
			efree(ptr);
		}
		return NULL;
	} else if (osize == 0) {
		return emalloc(nsize);
	} else {
		return erealloc(ptr, nsize);
	}
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

/** {{{ getLuaState */
static lua_State * getLuaState(zval * this_ptr)
{
	php_luasandbox_obj * intern = (php_luasandbox_obj*) 
		zend_object_store_get_object(this_ptr TSRMLS_CC);
	return intern->state;
}
/* }}} */

/** {{{ proto int LuaSandbox::loadString(string code, string chunkName) */
PHP_METHOD(LuaSandbox, loadString)
{
	char *code, *chunkName;
	int codeLength, chunkNameLength;
	int status;
	lua_State * L = getLuaState(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", 
				&code, &codeLength, &chunkName, &chunkNameLength) == FAILURE) {
		RETURN_FALSE;
	}

	// Check chunkName for nulls
	if (strlen(chunkName) != chunkNameLength) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, 
			"chunk name may not contain null characters");
		RETURN_FALSE;
	}

	// Find sandbox.scripts
	lua_getglobal(L, "sandbox");
	lua_getfield(L, -1, "scripts");

	// Parse the string into a function on the stack
	status = luaL_loadbuffer(L, code, codeLength, chunkName);
	if (status != 0) {
		luasandbox_handle_error(L, status);
		return;
	}
	
	// Store the resulting function as a member of sandbox.scripts
	lua_setfield(L, -2, chunkName);

	// Balance the stack
	lua_pop(L, 2);
	RETURN_TRUE;
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

/*** {{{ proto array LuaSandbox::callFunction(string name, ...)
 */
PHP_METHOD(LuaSandbox, callFunction)
{
	luasandbox_call_helper(INTERNAL_FUNCTION_PARAM_PASSTHRU, LUA_GLOBALSINDEX);
}
/* }}} */

/*** {{{ proto array LuaSandbox::callScript(string name, ...)
 */
PHP_METHOD(LuaSandbox, callScript)
{
	lua_State * L = getLuaState(getThis());
	lua_getglobal(L, "sandbox");
	lua_getfield(L, -1, "scripts");
	luasandbox_call_helper(INTERNAL_FUNCTION_PARAM_PASSTHRU, -1);
	lua_pop(L, 2);
}
/** }}} */

/** {{{ luasandbox_call_helper
 * Call a field or subfield of the table at the given index. Set return_value
 * to an array containing all the results.
 */
static void luasandbox_call_helper(INTERNAL_FUNCTION_PARAMETERS, int index)
{
	char *name;
	int nameLength = 0, status, origTop;
	zend_uint numArgs = 0, numResults, i;
	zval *** args = NULL;
	lua_State * L = getLuaState(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s*",
		&name, &nameLength, &args, &numArgs) == FAILURE)
	{
		RETURN_FALSE;
	}

	// Save the top position
	origTop = lua_gettop(L);

	// Find the function
	if (!luasandbox_find_field(L, index, name, nameLength)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"The specified lua function does not exist");
		RETVAL_FALSE;
		goto cleanup;
	}

	// Push the arguments
	for (i = 0; i < numArgs; i++) {
		if (!luasandbox_push_zval(L, *(args[i]))) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"Unable to convert argument %d to a lua value", i + 1);
			RETVAL_FALSE;
			goto cleanup;
		}
	}

	// Call it
	status = lua_pcall(L, numArgs, LUA_MULTRET, 0);
	if (status) {
		luasandbox_handle_error(L, status);
		goto cleanup;
	}

	// Calculate the number of results and create an array of that capacity
	numResults = lua_gettop(L) - origTop;
	array_init_size(return_value, numResults);

	// Fill the array with the results
	for (i = 0; i < numResults; i++) {
		zval * element = luasandbox_lua_to_zval(L, origTop + i + 1, NULL);
		zend_hash_next_index_insert(Z_ARRVAL_P(return_value), 
			(void*)&element,
			sizeof(zval*), NULL);
	}

cleanup:

	// Balance the stack
	lua_pop(L, lua_gettop(L) - origTop);
	// Delete varargs
	if (numArgs) {
		efree(args);
	}
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
			lua_gettable(L, top + 1);

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

/** {{{ luasandbox_push_zval */
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
			lua_pushlstring(L, p->arKey, p->nKeyLength);
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

/** {{{ luasandbox_method_getMemoryUsage */
static int luasandbox_method_getMemoryUsage(lua_State * L)
{
	php_luasandbox_obj * intern = luasandbox_get_php_obj(L);
	if ((size_t)(lua_Integer)intern->memory_usage != intern->memory_usage) {
		lua_pushnumber(L, (lua_Number)intern->memory_usage);
	} else {
		lua_pushinteger(L, (lua_Integer)intern->memory_usage);
	}
	return 1;
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
