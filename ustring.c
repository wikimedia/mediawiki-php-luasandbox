#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>

#include <unicode/utf.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>

#include "php.h"
#include "php_luasandbox.h"
#include "luasandbox_unicode.h"

#define LUASANDBOX_CHECK_ICU_ERROR(errorCode, cleanupCode)	{ \
			if( U_FAILURE(errorCode) ) { \
				char _luasandbox_errmsg[1024]; \
				snprintf( _luasandbox_errmsg, 1024, "Unicode handling error: %s", u_errorName(errorCode) ); \
				lua_pushstring( L, _luasandbox_errmsg ); \
				cleanupCode; \
				lua_error(L); \
			} \
			errorCode = U_ZERO_ERROR; \
		}

/******************   Prototypes   ******************/

int luasandbox_ustr_create(lua_State * L);
int luasandbox_ustr_len(lua_State * L);
int luasandbox_ustr_concat(lua_State * L);
int luasandbox_ustr_eq(lua_State * L);
int luasandbox_ustr_index(lua_State * L);

int luasandbox_ustr_ucfirst(lua_State * L);
int luasandbox_ustr_uc(lua_State * L);
int luasandbox_ustr_lc(lua_State * L);
int luasandbox_ustr_tc(lua_State * L);
int luasandbox_ustr_trim(lua_State * L);
int luasandbox_ustr_sub(lua_State * L);
int luasandbox_ustr_pos(lua_State * L);
int luasandbox_ustr_replace(lua_State * L);
int luasandbox_ustr_split(lua_State * L);

/******************   Registration of functions   ******************/

static luaL_Reg luasandbox_ustr_functions[] = {
	{ "len", luasandbox_ustr_len },
	{ "ucfirst", luasandbox_ustr_ucfirst },
	{ "uc", luasandbox_ustr_uc },
	{ "lc", luasandbox_ustr_lc },
	{ "tc", luasandbox_ustr_tc },
	{ "trim", luasandbox_ustr_trim },
	{ "sub", luasandbox_ustr_sub },
	{ "pos", luasandbox_ustr_pos },
	{ "replace", luasandbox_ustr_replace },
	{ "split", luasandbox_ustr_split },
	NULL
};

/** {{{ luasandbox_install_unicode_functions
 * 
 * Installs the unicode module into the global namespace.
 */
void luasandbox_install_unicode_functions(lua_State * L)
{
	luaL_newmetatable( L, "luasandbox_ustr" );

	lua_pushstring( L, "__len" );
	lua_pushcfunction( L, luasandbox_ustr_len );
	lua_rawset( L, -3 );

	lua_pushstring( L, "__concat" );
	lua_pushcfunction( L, luasandbox_ustr_concat );
	lua_rawset( L, -3 );

	lua_pushstring( L, "__eq" );
	lua_pushcfunction( L, luasandbox_ustr_eq );
	lua_rawset( L, -3 );

	lua_pushstring( L, "__index" );
	lua_pushcfunction( L, luasandbox_ustr_index );
	lua_rawset( L, -3 );

	lua_pushcfunction( L, luasandbox_ustr_create );
	lua_setglobal( L, "u" );

	luaL_register( L, "ustring", luasandbox_ustr_functions );
}
/* }}} */

/******************   Common functions   ******************/

/** {{{ luasandbox_init_ustr
 * 
 * Initializes a ustring header and assigns the metatable to it.
 */
luasandbox_ustr_header *luasandbox_init_ustr(lua_State * L, size_t len)
{
	luasandbox_ustr_header *result;
	
	result = (luasandbox_ustr_header*) lua_newuserdata( L, sizeof(luasandbox_ustr_header) + len );
	result->raw_len = len;

	luaL_getmetatable( L, "luasandbox_ustr" );
	lua_setmetatable( L, -2 );

	return result;
}
/* }}} */

/** {{{ luasandbox_push_ustr
 * 
 * Constructs the ustring object from a UTF-8 string. Validates the string and
 * raises an error if the string is invalid.
 */
luasandbox_ustr_header *luasandbox_push_ustr(lua_State * L, uint8_t *str, size_t len)
{
	luasandbox_ustr_header *header;
	int32_t i, cp_len;

	// Validate the string + calculate length
	for( i = cp_len = 0; i < len; cp_len++ ) {
		UChar32 cur;

		U8_NEXT( str, i, len, cur );
		if( cur < 0 ) {
			lua_pushstring( L, "Invalid UTF-8 supplied" );
			lua_error( L );
		}
	}

	header = luasandbox_init_ustr( L, len );
	header->cp_len = cp_len;
	memcpy( LUASANDBOX_USTR_RAW(header), str, len );

	return header;
}
/* }}} */

/** {{{ luasandbox_isustr
 * 
 * Checks if the the object on the stack is a ustring.
 */
int luasandbox_isustr(lua_State * L, int idx)
{
	int result;

	if( lua_type( L, idx ) != LUA_TUSERDATA )
		return FALSE;

	if( !lua_getmetatable( L, idx ) )
		return FALSE;

	luaL_getmetatable( L, "luasandbox_ustr" );

	result = lua_equal( L, -1, -2 );
	lua_pop( L, 2 );
	return result;
}
/* }}} */

/** {{{ luasandbox_checkustring
 * 
 * Checks whether the specified object on the stack is a ustring
 * or an object which may be converted to it. Returns the pointer
 * to the ustring's header.
 */
luasandbox_ustr_header* luasandbox_checkustring(lua_State * L, int idx)
{
	if ( lua_type( L, idx ) == LUA_TSTRING || lua_type( L, idx ) == LUA_TNUMBER ) {
		// A usual string. Magically convert it to ustring.
		lua_checkstack( L, 2 );
		lua_pushvalue( L, idx );
		luasandbox_ustr_create(L);
		lua_replace( L, idx );
		lua_pop( L, 1 );
	}

	return luaL_checkudata( L, idx, "luasandbox_ustr" );
}
/* }}} */

/** {{{ luasandbox_checkustring
 * 
 * Returns the pointer to the string itself and sets raw_len
 * to the length of string in bytes.
 */
const uint8_t* luasandbox_getustr(lua_State * L, int idx, size_t* raw_len)
{
	luasandbox_ustr_header *header;
	header = luasandbox_checkustring( L, idx );
	*raw_len = header->raw_len;
	return LUASANDBOX_USTR_RAW(header);
}
/* }}} */

/** {{{ luasandbox_ustr_index_to_offset
 * 
 * Converts a Lua index (starting with 1) to a C offset (starting with 0).
 * Handles negative indexes as indexes numbered from the end of the string.
 */
int32_t luasandbox_ustr_index_to_offset(lua_State * L, luasandbox_ustr_header *str, int32_t idx, int check_limits)
{
	if( !idx || check_limits && (idx > str->cp_len || -idx > str->cp_len) ) {
		lua_pushfstring( L, "Trying to access invalid index %d for string with length %d", idx, str->cp_len );
		lua_error( L );
	}

	if( idx > 0 ) {
		return idx - 1;
	} else {
		return str->cp_len + idx;
	}
}
/* }}} */

/******************   Conversions   ******************/

/** {{{ luasandbox_convert_toUTF16
 * 
 * Converts the specified ustring to UTF-16, and pushes
 * the resulting UTF-16 string on the top of the stack.
 */
void luasandbox_convert_toUTF16(lua_State * L, int idx)
{
	luasandbox_ustr_header *header;
	UChar *utf16_string;
	int32_t result_len;
	UErrorCode error_code = U_ZERO_ERROR;

	header = luasandbox_checkustring( L, idx );

	utf16_string = emalloc( header->raw_len * 2 );
	u_strFromUTF8( utf16_string, header->raw_len, &result_len,
		LUASANDBOX_USTR_RAW(header), header->raw_len, &error_code );
	LUASANDBOX_CHECK_ICU_ERROR( error_code, efree( utf16_string ) );

	lua_pushlstring( L, (char*)utf16_string, result_len * 2 );
	efree( utf16_string );
}
/* }}} */

/** {{{ luasandbox_convert_fromUTF16
 * 
 * Converts the specified UTF-16 string to UTF-8, and pushes
 * the resulting ustring on the top of the stack.
 */
void luasandbox_convert_fromUTF16(lua_State * L, int idx)
{
	luasandbox_ustr_header *header;
	uint8_t *utf8_string;
	UChar *utf16_string;
	size_t orig_len;
	int32_t result_len;
	UErrorCode error_code = U_ZERO_ERROR;

	utf16_string = (UChar*) lua_tolstring( L, idx, &orig_len );

	utf8_string = emalloc( orig_len );
	u_strToUTF8( utf8_string, orig_len, &result_len,
		utf16_string, orig_len / 2, &error_code );
	LUASANDBOX_CHECK_ICU_ERROR( error_code, efree( utf8_string ) );

	luasandbox_push_ustr( L, utf8_string, result_len );
	efree( utf8_string );
}
/* }}} */

/******************   Operators   ******************/

/** {{{ luasandbox_ustr_create
 * 
 * Initializes the Unicode string from the string on the top of the stack.
 */
int luasandbox_ustr_create(lua_State * L)
{
	uint8_t *str;
	size_t raw_len = 0;

	str = luaL_checklstring( L, -1, &raw_len );
	luasandbox_push_ustr( L, str, raw_len );
	return 1;
}
/* }}} */

/** {{{ luasandbox_ustr_len
 * 
 * Lua function providing the length of the string.
 */
int luasandbox_ustr_len(lua_State * L)
{
	luasandbox_ustr_header *header;

	header = luaL_checkudata( L, 1, "luasandbox_ustr" );

	lua_pushinteger( L, header->cp_len );
	return 1;
}
/* }}} */

/** {{{ luasandbox_ustr_concat
 * 
 * Lua function handling the concatention operator.
 */
int luasandbox_ustr_concat(lua_State * L)
{
	luasandbox_ustr_header *s1, *s2, *newhdr;
	int32_t new_len;
	void* newstr;
	
	s1 = luasandbox_checkustring( L, 1 );
	s2 = luasandbox_checkustring( L, 2 );
	
	new_len = s1->raw_len + s2->raw_len;
	newhdr = luasandbox_init_ustr( L, new_len );
	newhdr->cp_len = s1->cp_len + s2->cp_len;
	newstr = LUASANDBOX_USTR_RAW(newhdr);
	memcpy( newstr, LUASANDBOX_USTR_RAW(s1), s1->raw_len );
	memcpy( newstr + s1->raw_len, LUASANDBOX_USTR_RAW(s2), s2->raw_len );

	return 1;
}
/* }}} */

/** {{{ luasandbox_ustr_eq
 * 
 * Lua function providing the equality operator.
 */
int luasandbox_ustr_eq(lua_State * L)
{
	luasandbox_ustr_header *s1, *s2;

	s1 = luasandbox_checkustring( L, 1 );
	s2 = luasandbox_checkustring( L, 2 );

	if( s1->cp_len != s2->cp_len || s1->raw_len != s2->raw_len ) {
		lua_pushboolean( L, FALSE );
		return 1;
	}

	lua_pushboolean( L, !memcmp( LUASANDBOX_USTR_RAW(s1), LUASANDBOX_USTR_RAW(s2), s1->raw_len ) );
	return 1;
}
/* }}} */

/** {{{ luasandbox_ustr_index
 * 
 * Lua function providing the index operator.
 * Provides access both to class methods and
 * per-position access to string characters.
 */
int luasandbox_ustr_index(lua_State * L)
{
	luasandbox_ustr_header *str;
	uint8_t *raw;

	str = luaL_checkudata( L, 1, "luasandbox_ustr" );
	raw = LUASANDBOX_USTR_RAW(str);

	if( lua_type( L, 2 ) == LUA_TNUMBER ) {
		// If it is a number, treat as accessing string by position
		int32_t i, idx, curidx, offset;
		uint8_t* result_pos;
		UChar32 cur, result;

		idx = lua_tointeger( L, 2 );
		offset = luasandbox_ustr_index_to_offset( L, str, idx, TRUE );

		for( i = curidx = 0; ; curidx++ ) {
			UChar32 tmp;

			U8_GET_UNSAFE( raw, i, result );
			if( curidx == offset ) {
				result_pos = raw + i;
				break;
			}
			U8_NEXT_UNSAFE( raw, i, tmp );
		}

		lua_pushlstring( L, result_pos, U8_LENGTH( result ) );
		return 1;
	} else {
		// Otherwise treat it as an access to member functions
		lua_getglobal( L, "ustring" );
		lua_pushvalue( L, 2 );
		lua_gettable( L, -2 );
		return 1;
	}
}
/* }}} */

/******************   Library   ******************/

/** {{{ luasandbox_ustr_ucfirst
 * 
 * Lua function:
 *   ustring ucfirst( ustring str )
 * Converts the first code point of str to upper case.
 */
int luasandbox_ustr_ucfirst(lua_State * L)
{
	luasandbox_ustr_header *header;
	uint8_t *utf_string;
	size_t raw_len;
	UChar32 first, newfirst;
	int offset = 0;

	header = luaL_checkudata( L, 1, "luasandbox_ustr" );
	utf_string = LUASANDBOX_USTR_RAW( header );
	raw_len = header->raw_len;

	if( !raw_len ) {
		lua_pushstring( L, "" );
		return 1;
	}

	U8_GET_UNSAFE( utf_string, 0, first );

	newfirst = u_toupper( first );

	// The actions depend upon whether the lengths of symbol match
	if( U8_LENGTH(first) == U8_LENGTH(newfirst) ) {
		// Just replace the symbol
		luasandbox_ustr_header *newstr;
		uint8_t *result;
		
		newstr = lua_newuserdata( L, LUASANDBOX_USTR_TOTALLEN(header) ); 
		luaL_getmetatable( L, "luasandbox_ustr" );
		lua_setmetatable( L, -2 );

		memcpy( newstr, header, LUASANDBOX_USTR_TOTALLEN(header) );
		result = LUASANDBOX_USTR_RAW(newstr);
		U8_APPEND_UNSAFE( result, offset, newfirst );
	} else {
		// I have tested this code in cases when len(old) < len(new),
		// but I am unaware of any cases when those lengths do not match.
		// It should have happened with eszett, but since capital eszett is
		// considered substandard, u_toupper does not convert it.
		size_t oldlen = U8_LENGTH(first),
			newlen = U8_LENGTH(newfirst);
		size_t delta = newlen - oldlen;
		
		uint8_t *result;
		size_t new_len;

		result = emalloc( raw_len + delta );
		memcpy( result + newlen, utf_string + oldlen, raw_len - oldlen );
		U8_APPEND_UNSAFE( result, offset, newfirst );
		new_len = raw_len + delta;

		luasandbox_push_ustr( L, result, new_len );
		efree( result );
	}

	return 1;
}
/* }}} */

#define LUASANDBOX_UTF8_CHANGE_CASE_TOUPPER 1
#define LUASANDBOX_UTF8_CHANGE_CASE_TOLOWER 2
#define LUASANDBOX_UTF8_CHANGE_CASE_TOTITLE 3

/** {{{ luasandbox_ustr_change_case
 * 
 * Backend function for uc(), lc() and tc(). Converts string into UTF-16,
 * passes it to ICU function and then converts back to UTF-8. This is required
 * since casing algorithms are rather non-trivial and may be even locale-dependant.
 */
static int luasandbox_ustr_change_case(lua_State * L, int action)
{
	UChar *utf16_orig, *utf16_result;
	size_t orig_length, x;
	int32_t result_len;
	UErrorCode errorCode = U_ZERO_ERROR;

	luasandbox_convert_toUTF16( L, 1 );
	utf16_orig = (UChar*)lua_tolstring( L, -1, &orig_length );

	utf16_result = emalloc( orig_length * 2 );
	switch( action ) {
		case LUASANDBOX_UTF8_CHANGE_CASE_TOUPPER:
			result_len = u_strToUpper( utf16_result, orig_length, utf16_orig, orig_length / 2, "", &errorCode );
			break;
		case LUASANDBOX_UTF8_CHANGE_CASE_TOLOWER:
			result_len = u_strToLower( utf16_result, orig_length, utf16_orig, orig_length / 2, "", &errorCode );
			break;
		case LUASANDBOX_UTF8_CHANGE_CASE_TOTITLE:
			result_len = u_strToTitle( utf16_result, orig_length, utf16_orig, orig_length / 2, NULL, "", &errorCode );
			break;
	}
	LUASANDBOX_CHECK_ICU_ERROR( errorCode, efree(utf16_result) );
	lua_pop( L, 1 );	// Pop UTF-16 string out of the stack

	// Back to UTF-8
	lua_pushlstring( L, utf16_result, result_len * 2 );
	luasandbox_convert_fromUTF16( L, -1 );
	lua_replace( L, -2 );
	efree( utf16_result );

	return 1;
}
/* }}} */

int luasandbox_ustr_uc(lua_State * L)
{
	luasandbox_ustr_change_case( L, LUASANDBOX_UTF8_CHANGE_CASE_TOUPPER );
}

int luasandbox_ustr_lc(lua_State * L)
{
	luasandbox_ustr_change_case( L, LUASANDBOX_UTF8_CHANGE_CASE_TOLOWER );
}

int luasandbox_ustr_tc(lua_State * L)
{
	luasandbox_ustr_change_case( L, LUASANDBOX_UTF8_CHANGE_CASE_TOTITLE );
}

/** {{{ luasandbox_utf8_trim_lua
 * 
 * Lua function:
 *   ustring trim( ustring str )
 * Removes all the whitespace from the beginning and end of the string.
 */
int luasandbox_ustr_trim(lua_State * L)
{
	luasandbox_ustr_header *header, *newheader;
	uint8_t *utf_string, *result;
	size_t new_len;
	UChar32 cur;
	uint32_t i = 0, ltrim_len = 0, rtrim_len = 0, ltrim_len_cp = 0, rtrim_len_cp = 0;

	header = luasandbox_checkustring( L, 1 );
	utf_string = LUASANDBOX_USTR_RAW(header);

	// Left side
	while( i < header->raw_len ) {
		U8_NEXT_UNSAFE( utf_string, i, cur );

		if( u_isWhitespace( cur ) || u_isUWhiteSpace( cur ) ) {
			ltrim_len = i;
			ltrim_len_cp++;
		} else {
			break;
		}
	}
	// Right side
	while( i < header->raw_len ) {
		U8_NEXT_UNSAFE( utf_string, i, cur );

		if( u_isWhitespace( cur ) || u_isUWhiteSpace( cur ) ) {
			rtrim_len += U8_LENGTH( cur );
			rtrim_len_cp++;
		} else {
			rtrim_len = 0;
			rtrim_len_cp = 0;
		}
	}

	new_len = header->raw_len - ltrim_len - rtrim_len;
	newheader = luasandbox_init_ustr( L, new_len );
	newheader->cp_len = header->cp_len - ltrim_len_cp - rtrim_len_cp;
	memcpy( LUASANDBOX_USTR_RAW(newheader), utf_string + ltrim_len, new_len );
	
	return 1;
}
/* }}} */

/** {{{ luasandbox_ustr_sub
 * 
 * Lua function:
 *   ustring sub( ustring str, int offset[, int length] )
 * Returns the substring of str. Starts from the offset,
 * and returns at most length code points.
 */
int luasandbox_ustr_sub(lua_State * L)
{
	luasandbox_ustr_header *header;
	uint8_t *utf_string, *result;
	size_t len;
	
	int32_t i = 0, idx = 0, target = 0, target_len;
	int32_t target_start, target_end = -1;
	int found = 0;
	UChar32 cur;
	
	header = luasandbox_checkustring( L, 1 );
	utf_string = LUASANDBOX_USTR_RAW(header);
	target = luaL_checkinteger( L, 2 );
	if( lua_type( L, 3 ) == LUA_TNUMBER ) {
		target_len = lua_tointeger( L, 3 );
	} else {
		target_len = -1;
	}

	target = luasandbox_ustr_index_to_offset( L, header, target, TRUE );

	// Find the start symbol
	while( i < header->raw_len ) {
		if( idx == target ) {
			found = TRUE;
			break;
		}
		
		U8_NEXT_UNSAFE( utf_string, i, cur );
		idx++;
	}

	// If start symbol index is larger than string size, return null
	if( !found ) {
		lua_pushstring( L, "" );
		return 1;
	}

	target_start = i;
	idx = 0;

	// Find the end position
	while( i < header->raw_len ) {
		if( idx == target_len ) {
			target_end = i;
			break;
		}

		U8_NEXT_UNSAFE( utf_string, i, cur );
		idx++;
	}

	if( target_end == -1 ) {
		target_end = header->raw_len;
	}

	luasandbox_push_ustr( L, utf_string + target_start, target_end - target_start );
	return 1;
}
/* }}} */

/******************   Substring search and related operators. Beware.   ******************/

typedef struct {
	UChar32* string;	// UTF-32 representation of the needle string
	int32_t* table;	// KMP table
	int32_t length;	// Length of the needle string in code points
	int32_t raw_length;	// Length of the needle string in UTF-8 bytes
	int singleCharMode;	// Whether the needle string is a single character
} ustr_needle_string;

#define UTF8_SEARCH_STATUS_FOUND 1
#define UTF8_SEARCH_STATUS_NOTFOUND 0

typedef struct {
	int32_t status;		// Status of the search
	int32_t raw_index;	// Index in bytes
	int32_t cp_index;	// Index in codepoints
} ustr_search_result;

/** {{{ luasandbox_ustr_search_prepare
 * 
 * Preprocesses the string so a search may be performed on it using KMP algorithm.
 */
static ustr_needle_string* luasandbox_ustr_search_prepare(uint8_t* utf_string, int32_t raw_len)
{
	ustr_needle_string* str;
	int32_t i, idx;
	UChar32 cur;
	UErrorCode errorCode = U_ZERO_ERROR;
	int32_t cnd = 0;

	// Here we use the worst-case allocation
	str = emalloc( sizeof( ustr_needle_string ) );
	memset( str, 0, sizeof( ustr_needle_string ) );
	str->string = emalloc( raw_len * 4 );
	str->raw_length = raw_len;

	// Convert UTF-8 to UTF-32 for search purposes
	for( i = idx = 0; i < raw_len; idx++ ) {
		U8_NEXT_UNSAFE( utf_string, i, cur );
		str->string[idx] = cur;
	}
	str->length = idx;

	// KMP cannot handle single character search
	// (or it can, but my implementation cannot)
	// Use special case handler
	str->singleCharMode = str->length == 1;
	if( str->singleCharMode )
		return str;

	// Fill the search prefix table
	str->table = emalloc( str->length * sizeof(int32_t) );
	str->table[0] = -1;	// Yes, UChar32 is a signed type. "U" is for "Unicode", not for "unsigned"
	str->table[1] = 0;
	for( i = 2; i < str->length; i++ ) {
		if( str->string[i - 1] == str->string[cnd] ) {
			cnd++;
			str->table[i] = cnd;
		} else if( cnd > 0 ) {
			cnd = str->table[cnd];
			i--;
		} else {
			str->table[i] = 0;
		}
	}

	return str;
}

/** {{{ luasandbox_ustr_search_free
 * 
 * Frees the memory allocated for the preprocessed needle string.
 */
void luasandbox_ustr_search_free(ustr_needle_string *needle)
{
	if( needle->table )
		efree( needle->table );
	efree( needle->string );
	efree( needle );
}

#define UTF8_SEARCH_OFFSET_NONE 0
#define UTF8_SEARCH_OFFSET_RAW  1
#define UTF8_SEARCH_OFFSET_CP   2

/** {{{ luasandbox_ustr_search
 * 
 * Performs search of a substring in a string using the Knuth-Morris-Pratt algorithm.
 * Allows different types of start offset. The needle string must be preprocessed.
 */
ustr_search_result luasandbox_ustr_search(uint8_t *haystack, int32_t haystack_len, int offset_type, int offset, ustr_needle_string* needle) {
	int i, j, idx;	// Raw offset in haystack, CP offset in needle, CP offset in haystack
	UChar32 cur;
	ustr_search_result result;

	// Defaults
	result.raw_index = -1;
	result.cp_index  = -1;

	// If we are given raw offset, start with it
	if( offset_type == UTF8_SEARCH_OFFSET_RAW ) {
		i = offset;
	} else {
		i = 0;
	}

	if( needle->singleCharMode ) {
		// Handle special case of single character
		for( idx = 0; i < haystack_len; idx++ ) {
			U8_NEXT_UNSAFE( haystack, i, cur );

			if( offset_type == UTF8_SEARCH_OFFSET_CP && idx < offset )
				continue;

			if( needle->string[0] == cur ) {
				result.status = UTF8_SEARCH_STATUS_FOUND;
				result.cp_index = idx;
				result.raw_index = i - needle->raw_length;
				return result;
			}
		}
	} else {
		// Otherwise use KMP search
		for( j = idx = 0; i < haystack_len; idx++ ) {
			U8_NEXT_UNSAFE( haystack, i, cur );

			if( offset_type == UTF8_SEARCH_OFFSET_CP && idx < offset )
				continue;

			while( j > 0 && needle->string[j] != cur ) {
				j = needle->table[j];
			}
			if( needle->string[j] == cur )
				j++;
			if( j == needle->length ) {
				result.status = UTF8_SEARCH_STATUS_FOUND;
				result.cp_index = (idx+1) - needle->length;
				result.raw_index = i - needle->raw_length;
				return result;
			}
		}
	}

	result.status = UTF8_SEARCH_STATUS_NOTFOUND;
	return result;
}
/* }}} */

/** {{{ luasandbox_ustr_pos
 * 
 * Lua function
 *   int pos( ustring haystack, ustring needle[, int offset] )
 * Searches for a substring in a string. Returns an offset
 * according to Lua conventions (starting with 1).
 */
int luasandbox_ustr_pos(lua_State * L)
{
	luasandbox_ustr_header *header_haystack, *header_needle;
	uint8_t *haystack, *needle_raw;
	ustr_needle_string *needle;
	int32_t offset;
	ustr_search_result result;

	header_haystack = luasandbox_checkustring( L, 1 );
	header_needle = luasandbox_checkustring( L, 2 );

	haystack = LUASANDBOX_USTR_RAW(header_haystack);
	needle_raw = LUASANDBOX_USTR_RAW(header_needle);
	if( lua_type( L, 3 ) == LUA_TNUMBER ) {
		offset = lua_tointeger( L, 3 );
	} else {
		offset = 1;
	}

	offset = luasandbox_ustr_index_to_offset( L, header_haystack, offset, TRUE );

	if( !header_needle->raw_len ) {
		lua_pushstring( L, "The needle parameter may not be empty" );
		lua_error( L );
	}

	needle = luasandbox_ustr_search_prepare( needle_raw, header_needle->raw_len );

	result = luasandbox_ustr_search( haystack, header_haystack->raw_len, UTF8_SEARCH_OFFSET_CP, offset, needle );
	luasandbox_ustr_search_free( needle );

	switch( result.status ) {
		case UTF8_SEARCH_STATUS_FOUND:
			lua_pushinteger( L, result.cp_index + 1 );
			return 1;
		case UTF8_SEARCH_STATUS_NOTFOUND:
			lua_pushinteger( L, -1 );
			return 1;
	}
}
/* }}} */

/** {{{ luasandbox_ustr_replace
 * 
 * Lua function:
 *   replace( ustring haystack, ustring needle, ustring replacement[, int offset[, int limit]] )
 * Replaces at most limit occurances of needle in haystack with replacement,
 * starting at offset.
 */
int luasandbox_ustr_replace(lua_State * L)
{
	luasandbox_ustr_header *header_haystack, *header_needle, *header_replacement, *header_result;
	uint8_t *haystack, *needle_raw, *replacement, *result;
	size_t haystack_len, needle_len, replacement_len, result_len;
	ustr_needle_string *needle;
	ustr_search_result cur;
	int32_t i, offset, offset_src, offset_dest, matches_num, limit;
	int32_t *matches;
	int offset_mode;

	header_haystack = luasandbox_checkustring( L, 1 );
	header_needle = luasandbox_checkustring( L, 2 );
	header_replacement = luasandbox_checkustring( L, 3 );

	haystack = LUASANDBOX_USTR_RAW(header_haystack);
	haystack_len = header_haystack->raw_len;
	needle_raw = LUASANDBOX_USTR_RAW(header_needle);
	needle_len = header_needle->raw_len;
	replacement = LUASANDBOX_USTR_RAW(header_replacement);
	replacement_len = header_replacement->raw_len;

	if( lua_type( L, 4 ) == LUA_TNUMBER ) {
		offset = lua_tointeger( L, 4 );
		offset = luasandbox_ustr_index_to_offset( L, header_haystack, offset, TRUE );
		offset_mode = UTF8_SEARCH_OFFSET_CP;
	} else {
		offset = 0;
		offset_mode = UTF8_SEARCH_OFFSET_RAW;
	}
	limit = ( lua_type( L, 5 ) == LUA_TNUMBER ) ?
		luaL_checkinteger( L, 5 ) :
		-1;

	if( !needle_len ) {
		lua_pushstring( L, "The needle parameter may not be empty" );
		lua_error( L );
	}

	needle = luasandbox_ustr_search_prepare( needle_raw, needle_len );

	// As usually, just use worst-case scenario for memory allocation
	matches = emalloc( ( haystack_len / needle_len + 1 ) * sizeof(int32_t) );

	// Find all substrings to repalce
	matches_num = 0;
	for(;;) {
		if( limit > 0 && matches_num >= limit ) {
			break;
		}

		cur = luasandbox_ustr_search( haystack, haystack_len, offset_mode, offset, needle );

		if( cur.status == UTF8_SEARCH_STATUS_FOUND ) {
			matches[matches_num] = cur.raw_index;
			matches_num++;
			offset = cur.raw_index + needle->raw_length;
			offset_mode = UTF8_SEARCH_OFFSET_RAW;
		} else {
			break;
		}
	}
	luasandbox_ustr_search_free( needle );

	if( !matches_num ) {
		lua_pushvalue( L, 1 );
		return 1;
	}

	// Initialize the resulting string
	result_len = haystack_len + ( replacement_len - needle_len ) * matches_num;
	header_result = luasandbox_init_ustr( L, result_len );
	header_result->cp_len = header_haystack->cp_len +
		( header_replacement->raw_len - header_needle->raw_len ) * matches_num;
	result = LUASANDBOX_USTR_RAW(header_result);

	// Replace all substrings
	memcpy( result, haystack, matches[i] );
	offset_src = offset_dest = matches[i];
	for( i = 0; i < matches_num; i++ ) {
		int32_t postfix_len;

		memcpy( result + offset_dest, replacement, replacement_len );
		offset_src  += needle_len;
		offset_dest += replacement_len;

		if( i == matches_num - 1 ) {
			postfix_len = haystack_len - offset_src;
		} else {
			postfix_len = matches[i+1] - offset_src;
		}

		memcpy( result + offset_dest, haystack + offset_src, postfix_len );
		offset_src  += postfix_len;
		offset_dest += postfix_len;
	}

	efree( matches );

	return 1;
}
/* }}} */

/** {{{ luasandbox_ustr_split
 * 
 * Lua function:
 *   split( ustring haystack, ustring separator[, int limit] )
 * 
 */
int luasandbox_ustr_split(lua_State * L)
{
	luasandbox_ustr_header *header_haystack, *header_needle;
	uint8_t *haystack, *needle_raw;
	size_t haystack_len, needle_len;
	ustr_needle_string *needle;
	ustr_search_result cur;
	int32_t i, offset, matches_num, limit;
	int32_t *matches;

	header_haystack = luasandbox_checkustring( L, 1 );
	header_needle = luasandbox_checkustring( L, 2 );

	haystack = LUASANDBOX_USTR_RAW(header_haystack);
	needle_raw = LUASANDBOX_USTR_RAW(header_needle);
	haystack_len = header_haystack->raw_len;
	needle_len = header_needle->raw_len;

	limit = ( lua_tointeger( L, 3 ) == LUA_TNUMBER ) ?
		luaL_checkinteger( L, 3 ) :
		-1;

	if( !needle_len ) {
		lua_pushstring( L, "The needle parameter may not be empty" );
		lua_error( L );
	}

	needle = luasandbox_ustr_search_prepare( needle_raw, needle_len );
	if( !needle ) {
		LUASANDBOX_UNICODE_INVALID_FAIL();
	}

	// As usually, just use worst-case scenario for memory allocation
	matches = emalloc( ( haystack_len / needle_len + 1 ) * sizeof(int32_t) );

	// Find all substrings to split
	matches_num = 0;
	offset = 0;
	for(;;) {
		if( limit > 0 && matches_num >= limit ) {
			break;
		}

		cur = luasandbox_ustr_search( haystack, haystack_len, UTF8_SEARCH_OFFSET_RAW, offset, needle );

		if( cur.status == UTF8_SEARCH_STATUS_FOUND ) {
			matches[matches_num] = cur.raw_index;
			matches_num++;
			offset = cur.raw_index + needle->raw_length;
		} else {
			break;
		}
	}
	luasandbox_ustr_search_free( needle );

	lua_createtable( L, matches_num + 1, 0 );

	if( !matches_num ) {
		lua_pushlstring( L, haystack, haystack_len );
		lua_rawseti( L, -2, 1 );
		return 1;
	}

	// Push all matches into the table
	lua_pushlstring( L, haystack, matches[0] );
	lua_rawseti( L, -2, 1 );
	offset = matches[0];
	for( i = 0; i < matches_num; i++ ) {
		int32_t bit_len;

		offset += needle_len;

		if( i == matches_num - 1 ) {
			bit_len = haystack_len - offset;
		} else {
			bit_len = matches[i+1] - offset;
		}

		lua_pushlstring( L, haystack + offset, bit_len );
		lua_rawseti( L, -2, i + 2 );
		offset += bit_len;
	}

	return 1;
}
/* }}} */
