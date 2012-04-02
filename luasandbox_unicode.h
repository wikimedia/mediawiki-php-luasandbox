#ifndef LUASANDBOX_UNICODE_H
#define LUASANDBOX_UNICODE_H

#include <stdint.h>
#include <lua.h>

/**
 * Unicode string are input and stored as UTF-8.
 */
typedef struct {
	size_t raw_len;	// Byte length in UTF-8
	int32_t cp_len;	// Amount of code points
} luasandbox_ustr_header;

#define LUASANDBOX_USTR_RAW(header) ((uint8_t*) ( ((void*)header) + sizeof(luasandbox_ustr_header) ))
#define LUASANDBOX_USTR_TOTALLEN(header) ( sizeof(luasandbox_ustr_header) + header->raw_len )

void luasandbox_install_unicode_functions(lua_State * L);

luasandbox_ustr_header *luasandbox_init_ustr(lua_State * L, size_t len);
luasandbox_ustr_header *luasandbox_push_ustr(lua_State * L, uint8_t *str, size_t len);
int luasandbox_isustr(lua_State * L, int idx);
luasandbox_ustr_header* luasandbox_checkustring(lua_State * L, int idx);
const uint8_t* luasandbox_getustr(lua_State * L, int idx, size_t* raw_len);
int32_t luasandbox_ustr_index_to_offset(lua_State * L, luasandbox_ustr_header *str, int32_t idx, int check_limits);

void luasandbox_convert_toUTF16(lua_State * L, int idx);
void luasandbox_convert_fromUTF16(lua_State * L, int idx);

#endif
