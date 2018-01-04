/**
 * The Lua allocator hook
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>

#include "php.h"
#include "php_luasandbox.h"

static inline int luasandbox_update_memory_accounting(php_luasandbox_alloc * obj,
	size_t osize, size_t nsize);
static void *luasandbox_php_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

lua_State * luasandbox_alloc_new_state(php_luasandbox_alloc * alloc, php_luasandbox_obj * sandbox)
{
	lua_State * L;
	L = lua_newstate(luasandbox_php_alloc, sandbox);
	return L;
}

void luasandbox_alloc_delete_state(php_luasandbox_alloc * alloc, lua_State * L)
{
	lua_close(L);
}


/** {{{ luasandbox_update_memory_accounting
 *
 * Update memory usage statistics for the given memory allocation request.
 * Returns 1 if the allocation should be allowed, 0 if it should fail.
 */
static inline int luasandbox_update_memory_accounting(php_luasandbox_alloc * alloc,
	size_t osize, size_t nsize)
{
	if (nsize > osize && (nsize > alloc->memory_limit
		|| alloc->memory_usage + nsize > alloc->memory_limit))
	{
		// Memory limit exceeded
		return 0;
	}

	if (osize > nsize && alloc->memory_usage + nsize < osize) {
		// Negative memory usage -- do not update
		return 1;
	}

	alloc->memory_usage += nsize - osize;
	if (alloc->memory_usage > alloc->peak_memory_usage) {
		alloc->peak_memory_usage = alloc->memory_usage;
	}
	return 1;
}
/* }}} */

/** {{{ luasandbox_php_alloc
 *
 * The Lua allocator function. Use PHP's request-local allocator as a backend.
 * Account for memory usage and deny the allocation request if the amount
 * allocated is above the user-specified limit.
 */
static void *luasandbox_php_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	php_luasandbox_obj * obj = (php_luasandbox_obj*)ud;
	void * nptr;
	obj->in_php ++;
	if (!luasandbox_update_memory_accounting(&obj->alloc, osize, nsize)) {
		obj->in_php --;
		return NULL;
	}

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
	obj->in_php --;
	return nptr;
}
/* }}} */
