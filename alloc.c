/**
 * The Lua allocator hook
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <limits.h>

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

/** {{{ luasandbox_update_gc_pause
 * Scale the GC pause size so that collection will start before an OOM occurs (T349462)
 */
static inline void luasandbox_update_gc_pause(lua_State * L, php_luasandbox_alloc * alloc)
{
	size_t limit = alloc->memory_limit;
	size_t usage = alloc->memory_usage;

	// Guard against overflow and division by zero
	if (limit >= SIZE_MAX / 90 || usage == 0) {
		return;
	}
	size_t pause = limit * 90 / usage;
	if (pause > 200) {
		pause = 200;
	}
	lua_gc(L, LUA_GCSETPAUSE, (int)pause);
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

	luasandbox_update_gc_pause(obj->state, &obj->alloc);

	if (nsize == 0) {
		if (ptr) {
			efree(ptr);
		}
		nptr = NULL;
	} else if (osize == 0) {
		nptr = ecalloc(1, nsize);
	} else {
		nptr = erealloc(ptr, nsize);
		if (nsize > osize) {
			memset(nptr + osize, 0, nsize - osize);
		}
	}
	obj->in_php --;
	return nptr;
}
/* }}} */
