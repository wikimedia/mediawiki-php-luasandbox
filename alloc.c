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

#if defined(LUA_JITLIBNAME) && (SSIZE_MAX >> 32) > 0
#define LUASANDBOX_LJ_64
#endif

static inline int luasandbox_update_memory_accounting(php_luasandbox_alloc * obj, 
	size_t osize, size_t nsize, int in_lua);
#ifdef LUASANDBOX_LJ_64
static void *luasandbox_passthru_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
#else
static void *luasandbox_php_alloc(void *ud, void *ptr, size_t osize, size_t nsize);
#endif

lua_State * luasandbox_alloc_new_state(php_luasandbox_alloc * alloc, php_luasandbox_obj * sandbox)
{
	lua_State * L;
#ifdef LUASANDBOX_LJ_64
	// The 64-bit version of LuaJIT needs to use its own allocator
	L = luaL_newstate();
	if (L) {
		alloc->old_alloc = lua_getallocf(L, &alloc->old_alloc_ud);
		lua_setallocf(L, luasandbox_passthru_alloc, sandbox);
	}
#else
	L = lua_newstate(luasandbox_php_alloc, sandbox);
#endif
	return L;
}

void luasandbox_alloc_delete_state(php_luasandbox_alloc * alloc, lua_State * L)
{
	// In 64-bit LuaJIT mode, restore the old allocator before calling 
	// lua_close() because lua_close() actually checks that the value of the 
	// function pointer is unchanged before destroying the underlying 
	// allocator. If the allocator has been changed, the mmap is not freed.
#ifdef LUASANDBOX_LJ_64
	lua_setallocf(L, alloc->old_alloc, alloc->old_alloc_ud);
#endif

	lua_close(L);
}


/** {{{ luasandbox_update_memory_accounting
 *
 * Update memory usage statistics for the given memory allocation request.
 * Returns 1 if the allocation should be allowed, 0 if it should fail.
 */
static inline int luasandbox_update_memory_accounting(php_luasandbox_alloc * alloc, 
	size_t osize, size_t nsize, int in_lua)
{
	// Allow some extra memory overhead for non-in_lua allocations to avoid
	// getting into luasandbox_panic due to allocation failures.
	size_t slop = in_lua ? 0 : 1024*1024;

	if (nsize > alloc->memory_limit
		|| alloc->memory_usage > alloc->memory_limit + slop - nsize)
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

#ifndef LUASANDBOX_LJ_64
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
	if (!luasandbox_update_memory_accounting(&obj->alloc, osize, nsize, obj->in_lua)) {
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
#endif

#ifdef LUASANDBOX_LJ_64
/** {{{ luasandbox_passthru_alloc
 *
 * A Lua allocator function for use with LuaJIT on a 64-bit platform. Pass 
 * allocation requests through to the standard allocator, which is customised
 * on this platform to always return memory from the lower 2GB of address 
 * space.
 */
static void *luasandbox_passthru_alloc(void *ud, void *ptr, size_t osize, size_t nsize) 
{
	php_luasandbox_obj * obj = (php_luasandbox_obj*)ud;
	if (!luasandbox_update_memory_accounting(&obj->alloc, osize, nsize, obj->in_lua)) {
		return NULL;
	}
	return obj->alloc.old_alloc(obj->alloc.old_alloc_ud, ptr, osize, nsize);
}
/* }}} */
#endif
