// Compatibility helpers for the Lua C API versions supported by LuaSandbox.
#ifndef LUASANDBOX_LUA_COMPAT_H
#define LUASANDBOX_LUA_COMPAT_H

#include <limits.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

// The shim is written against the documented 5.1 and 5.2+ APIs rather than
// against a fixed list of versions, but only 5.1 and 5.4 are tested.
#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 501
#error "LuaSandbox requires Lua 5.1 or later"
#endif

#ifndef LUA_GNAME
#define LUA_GNAME "_G"
#endif

#ifndef LUA_QL
#define LUA_QL(x) "'" x "'"
#endif

#ifndef LUA_QS
#define LUA_QS LUA_QL("%s")
#endif

#ifndef LUA_INTFRMLEN
#ifdef LUA_INTEGER_FRMLEN
#define LUA_INTFRMLEN LUA_INTEGER_FRMLEN
#else
#define LUA_INTFRMLEN "l"
#endif
#endif

#ifndef LUA_INTFRM_T
#ifdef LUA_INTEGER
#define LUA_INTFRM_T LUA_INTEGER
#else
#define LUA_INTFRM_T long
#endif
#endif

#ifndef LUA_MAXCAPTURES
#define LUA_MAXCAPTURES 32
#endif

#if LUA_VERSION_NUM >= 502
static inline int luasandbox_luaL_checkint(lua_State *L, int narg)
{
	lua_Integer value = luaL_checkinteger(L, narg);
	luaL_argcheck(L, value >= INT_MIN && value <= INT_MAX, narg,
		"integer out of range");
	return (int)value;
}

static inline int luasandbox_luaL_optint(lua_State *L, int narg, int def)
{
	return lua_isnoneornil(L, narg) ? def : luasandbox_luaL_checkint(L, narg);
}

static inline int luasandbox_luaL_getn(lua_State *L, int index)
{
	size_t len = lua_rawlen(L, index);
	if (len > INT_MAX) {
		luaL_error(L, "object length is too large");
	}
	return (int)len;
}

#define luaL_checkint luasandbox_luaL_checkint
#define luaL_optint luasandbox_luaL_optint
#define luaL_getn luasandbox_luaL_getn
#endif

#if LUA_VERSION_NUM < 502
static inline int luasandbox_lua_absindex(lua_State *L, int index)
{
	if (index > 0 || index <= LUA_REGISTRYINDEX) {
		return index;
	}
	return lua_gettop(L) + index + 1;
}

static inline void luasandbox_pushglobaltable(lua_State *L)
{
	lua_pushvalue(L, LUA_GLOBALSINDEX);
}

static inline void luasandbox_luaL_requiref(lua_State *L, const char *name,
	lua_CFunction openf)
{
	(void)name;
	lua_pushcfunction(L, openf);
	lua_call(L, 0, 0);
}

#else
#define luasandbox_lua_absindex lua_absindex
#define luasandbox_pushglobaltable lua_pushglobaltable

static inline void luasandbox_luaL_requiref(lua_State *L, const char *name,
	lua_CFunction openf)
{
	luaL_requiref(L, name, openf, 1);
	lua_pop(L, 1);
}

static inline int luasandbox_lua_cpcall(lua_State *L, lua_CFunction func, void *ud)
{
	int status;
	lua_pushcfunction(L, func);
	lua_pushlightuserdata(L, ud);
	status = lua_pcall(L, 1, 0, 0);
	return status;
}

#define lua_cpcall luasandbox_lua_cpcall
#endif

#if LUA_VERSION_NUM < 502
#define luasandbox_luaL_register luaL_register
#else
static inline void luasandbox_luaL_register(lua_State *L, const char *libname,
	const luaL_Reg *funcs)
{
	lua_newtable(L);
	luaL_setfuncs(L, funcs, 0);
	if (libname) {
		lua_pushvalue(L, -1);
		lua_setglobal(L, libname);
	}
}
#endif

static inline int luasandbox_luaL_loadbuffer(lua_State *L, const char *buff,
	size_t size, const char *name, int binary)
{
#if LUA_VERSION_NUM >= 502
	return luaL_loadbufferx(L, buff, size, name, binary ? "b" : "t");
#else
	(void)binary;
	return luaL_loadbuffer(L, buff, size, name);
#endif
}

static inline int luasandbox_lua_dump(lua_State *L, lua_Writer writer, void *data)
{
#if LUA_VERSION_NUM >= 504
	return lua_dump(L, writer, data, 0);
#else
	return lua_dump(L, writer, data);
#endif
}

#endif // LUASANDBOX_LUA_COMPAT_H
