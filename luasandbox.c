#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <lua.h>
#include <lauxlib.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>

#include "php.h"
#include "php_ini.h"

#include "ext/pcre/php_pcre.h"

#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "php_luasandbox.h"
#include "luasandbox_timer.h"
#if PHP_VERSION_ID < 70000
#include "ext/standard/php_smart_str.h"
#else
#include "zend_smart_str.h"
#include "ext/standard/php_string.h"
#endif
#include "luasandbox_version.h"

// Compatability for PHP <= 5.3.6
#ifndef ZEND_FE_END
#define ZEND_FE_END { NULL, NULL, NULL, 0, 0 }
#endif

// Compatability typedefs and defines to hide some PHP5/PHP7 differences
#if PHP_VERSION_ID < 70000
typedef zend_object_value object_constructor_ret_t;
typedef int str_param_len_t;
typedef long long_param_t;
typedef zval*** star_param_t;
#define compat_zend_register_internal_class_ex(ce, parent_ce) zend_register_internal_class_ex(ce, parent_ce, NULL TSRMLS_CC)
#define compat_add_assoc_string(assoc, key, val) add_assoc_string((assoc), (key), (val), 1)
#else
typedef zend_object* object_constructor_ret_t;
typedef size_t str_param_len_t;
typedef zend_long long_param_t;
typedef zval* star_param_t;
#define compat_zend_register_internal_class_ex(ce, parent_ce) zend_register_internal_class_ex(ce, parent_ce)
#define compat_add_assoc_string(assoc, key, val) add_assoc_string((assoc), (key), (val))
#endif

#define CHECK_VALID_STATE(state) \
	if (!state) { \
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid LuaSandbox state"); \
		RETURN_FALSE; \
	}

static PHP_GINIT_FUNCTION(luasandbox);
static PHP_GSHUTDOWN_FUNCTION(luasandbox);
static object_constructor_ret_t luasandbox_new(zend_class_entry *ce TSRMLS_DC);
static lua_State * luasandbox_newstate(php_luasandbox_obj * intern TSRMLS_DC);
static void luasandbox_free_storage(zend_object *object TSRMLS_DC);
static object_constructor_ret_t luasandboxfunction_new(zend_class_entry *ce TSRMLS_DC);
static void luasandboxfunction_free_storage(zend_object *object TSRMLS_DC);
static int luasandbox_panic(lua_State * L);
static lua_State * luasandbox_state_from_zval(zval * this_ptr TSRMLS_DC);
static void luasandbox_load_helper(int binary, INTERNAL_FUNCTION_PARAMETERS);
static int luasandbox_find_field(lua_State * L, int index,
	char * spec, int specLength);
static void luasandbox_set_timespec(struct timespec * dest, double source);
static int luasandbox_function_init(zval * this_ptr, php_luasandboxfunction_obj ** pfunc,
	lua_State ** pstate, php_luasandbox_obj ** psandbox TSRMLS_DC);
static void luasandbox_function_push(php_luasandboxfunction_obj * pfunc, lua_State * pstate);
static void luasandbox_call_helper(lua_State * L, zval * sandbox_zval,
	php_luasandbox_obj * sandbox,
	star_param_t args, int numArgs, zval * return_value TSRMLS_DC);
static void luasandbox_handle_error(php_luasandbox_obj * sandbox, int status TSRMLS_DC);
static int luasandbox_dump_writer(lua_State * L, const void * p, size_t sz, void * ud);
static zend_bool luasandbox_instanceof(
	zend_class_entry *child_class, zend_class_entry *parent_class);

extern char luasandbox_timeout_message[];

/** For LuaSandbox::getProfilerFunctionReport() */
enum {
	LUASANDBOX_SAMPLES,
	LUASANDBOX_SECONDS,
	LUASANDBOX_PERCENT
};

zend_class_entry *luasandbox_ce;
zend_class_entry *luasandboxerror_ce;
zend_class_entry *luasandboxruntimeerror_ce;
zend_class_entry *luasandboxfatalerror_ce;
zend_class_entry *luasandboxsyntaxerror_ce;
zend_class_entry *luasandboxmemoryerror_ce;
zend_class_entry *luasandboxerrorerror_ce;
zend_class_entry *luasandboxtimeouterror_ce;
zend_class_entry *luasandboxemergencytimeouterror_ce;
zend_class_entry *luasandboxfunction_ce;

ZEND_DECLARE_MODULE_GLOBALS(luasandbox);

#if PHP_VERSION_ID >= 70000
static zend_object_handlers luasandbox_object_handlers;
static zend_object_handlers luasandboxfunction_object_handlers;
#endif

/** {{{ arginfo */
ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_getVersionInfo, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_allowedGlobals, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_additionalLibraries, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_luasandbox_loadString, 0, 0, 1)
	ZEND_ARG_INFO(0, code)
	ZEND_ARG_INFO(0, chunkName)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_luasandbox_loadBinary, 0, 0, 1)
	ZEND_ARG_INFO(0, code)
	ZEND_ARG_INFO(0, chunkName)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setMemoryLimit, 0)
	ZEND_ARG_INFO(0, limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_getMemoryUsage, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_getPeakMemoryUsage, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_setCPULimit, 0)
	ZEND_ARG_INFO(0, limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_getCPUUsage, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_pauseUsageTimer, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_unpauseUsageTimer, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_luasandbox_enableProfiler, 0, 0, 0)
	ZEND_ARG_INFO(0, period)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_disableProfiler, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_luasandbox_getProfilerFunctionReport, 0, 0, 0)
	ZEND_ARG_INFO(0, units)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_luasandbox_callFunction, 0, 0, 1)
	ZEND_ARG_INFO(0, name)
#ifdef ZEND_ARG_VARIADIC_INFO
	ZEND_ARG_VARIADIC_INFO(0, args)
#else
	ZEND_ARG_INFO(0, ...)
#endif
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_wrapPhpFunction, 0)
	ZEND_ARG_INFO(0, function)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandbox_registerLibrary, 0)
	ZEND_ARG_INFO(0, libname)
	ZEND_ARG_INFO(0, functions)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandboxfunction___construct, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_luasandboxfunction_call, 0, 0, 0)
#ifdef ZEND_ARG_VARIADIC_INFO
	ZEND_ARG_VARIADIC_INFO(0, args)
#else
	ZEND_ARG_INFO(0, ...)
#endif
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_luasandboxfunction_dump, 0)
ZEND_END_ARG_INFO()

/* }}} */

/** {{{ function entries */
const zend_function_entry luasandbox_functions[] = {
	ZEND_FE_END	/* Must be the last line in luasandbox_functions[] */
};

const zend_function_entry luasandbox_methods[] = {
	PHP_ME(LuaSandbox, getVersionInfo, arginfo_luasandbox_getVersionInfo, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	PHP_ME(LuaSandbox, allowedGlobals, arginfo_luasandbox_allowedGlobals, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	PHP_ME(LuaSandbox, additionalLibraries, arginfo_luasandbox_additionalLibraries, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)	
	PHP_ME(LuaSandbox, loadString, arginfo_luasandbox_loadString, 0)
	PHP_ME(LuaSandbox, loadBinary, arginfo_luasandbox_loadBinary, 0)
	PHP_ME(LuaSandbox, setMemoryLimit, arginfo_luasandbox_setMemoryLimit, 0)
	PHP_ME(LuaSandbox, getMemoryUsage, arginfo_luasandbox_getMemoryUsage, 0)
	PHP_ME(LuaSandbox, getPeakMemoryUsage, arginfo_luasandbox_getPeakMemoryUsage, 0)
	PHP_ME(LuaSandbox, setCPULimit, arginfo_luasandbox_setCPULimit, 0)
	PHP_ME(LuaSandbox, getCPUUsage, arginfo_luasandbox_getCPUUsage, 0)
	PHP_ME(LuaSandbox, pauseUsageTimer, arginfo_luasandbox_pauseUsageTimer, 0)
	PHP_ME(LuaSandbox, unpauseUsageTimer, arginfo_luasandbox_unpauseUsageTimer, 0)
	PHP_ME(LuaSandbox, enableProfiler, arginfo_luasandbox_enableProfiler, 0)
	PHP_ME(LuaSandbox, disableProfiler, arginfo_luasandbox_disableProfiler, 0)
#ifdef HHVM
	// We need to wrap this method in ext_luasandbox.php for HHVM
	PHP_ME(LuaSandbox, _internal_getProfilerFunctionReport, arginfo_luasandbox_getProfilerFunctionReport,
		ZEND_ACC_PRIVATE | ZEND_ACC_FINAL)
#else
	PHP_ME(LuaSandbox, getProfilerFunctionReport, arginfo_luasandbox_getProfilerFunctionReport, 0)
#endif
	PHP_ME(LuaSandbox, callFunction, arginfo_luasandbox_callFunction, 0)
	PHP_ME(LuaSandbox, wrapPhpFunction, arginfo_luasandbox_wrapPhpFunction, 0)
	PHP_ME(LuaSandbox, registerLibrary, arginfo_luasandbox_registerLibrary, 0)
	ZEND_FE_END
};

const zend_function_entry luasandboxfunction_methods[] = {
	PHP_ME(LuaSandboxFunction, __construct, arginfo_luasandboxfunction___construct,
		ZEND_ACC_PRIVATE | ZEND_ACC_FINAL)
	PHP_ME(LuaSandboxFunction, call, arginfo_luasandboxfunction_call, 0)
	PHP_ME(LuaSandboxFunction, dump, arginfo_luasandboxfunction_dump, 0)
	ZEND_FE_END
};

const zend_function_entry luasandbox_empty_methods[] = {
	ZEND_FE_END
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
	PHP_RSHUTDOWN(luasandbox), /* RSHUTDOWN */
	PHP_MINFO(luasandbox),
	LUASANDBOX_VERSION,
	PHP_MODULE_GLOBALS(luasandbox),
	PHP_GINIT(luasandbox),
	PHP_GSHUTDOWN(luasandbox),
	NULL, /* luasandbox_post_deactivate */
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_LUASANDBOX
ZEND_GET_MODULE(luasandbox)
#endif

/* php.ini: luasandbox.allowed_globals. */
/* {{{ register_allowed_globals
 */
static void register_allowed_globals( HashTable *table, const char *list TSRMLS_DC) {
	if ( table->nNumOfElements ) {
		/* Clean up old values, if any. */
		zend_hash_destroy( table );
	}
	/* Count the allowed globals to initialise the hashtable once and fo all .*/
	/* Inspired by https://stackoverflow.com/a/9210560. */
	size_t count = 0;
	char delim = ',';
	/* Count how many elements will be extracted. */
	const char* tmp1 = list;
	while (*tmp1) {
		if (delim == *tmp1++) {
			count++;
		}
	}
	/* And one for the element after the last comma. */
	count++;
	zend_hash_init( table, count, NULL, NULL, 1 );

	/* Inspired by https://github.com/GoogleCloudPlatform/stackdriver-debugger-php-extension/blob/master/stackdriver_debugger_ast.c */
	char *key = NULL, *last = NULL;
	int len = strlen( list );
	/* list needs to be duplicated, because php_strtok_r is destructive. */
	char *tmp2 = estrndup( list, len );
	for (key = php_strtok_r( tmp2, ",", &last); key; key = php_strtok_r( NULL, ",", &last ) ) {
		zend_string * key_z = zend_string_init( key, strlen( key ), 0 );
		char * trimmed = ZSTR_VAL(php_trim( key_z, NULL, 0, 3 ));
		zend_string_release( key_z );
#if PHP_VERSION_ID < 70000
		zend_hash_update(table,	trimmed, strlen( trimmed ) + 1,	(void*)"", 1, NULL);
#else
		zend_hash_str_add_empty_element( table, trimmed, strlen( trimmed ) );
#endif
	}
	efree( tmp2 );
}
/* }}} */

/* {{{ PHP_INI_MH(luasandbox_update_allowed_globals)
 */
static PHP_INI_MH(luasandbox_update_allowed_globals) {

    if ( new_value != NULL ) {

		/* Cast new value from php.ini or ini_set() to *char, if necessary. */
		zend_string * list;
#if PHP_VERSION_ID < 70000
		list = zend_string_init( new_value, strlen( new_value ), 0 );
#else
		list = new_value;
#endif

		/* Fill up the hashtable . */
		/* Leave this until php_explode() stops segfaulting.
		zval * exploded;
		array_init( exploded );
		exploded = php_explode( delim, list, exploded, ZEND_LONG_MAX );
		LUASANDBOX_G(allowed_globals) = *Z_ARRVAL_P(exploded);
		*/
		register_allowed_globals( &LUASANDBOX_G(allowed_globals), ZSTR_VAL(new_value) TSRMLS_CC );

		return SUCCESS;
	}
}
/* }}} */

/* {{{ PHP_INI_MH(luasandbox_update_additional_libraries)
 */
static PHP_INI_MH(luasandbox_update_additional_libraries) {
    if ( new_value != NULL ) {
		if ( LUASANDBOX_G(additional_libraries).nNumOfElements ) {
			/* Clean up old values, if any. */
			zend_hash_destroy( &LUASANDBOX_G(additional_libraries) );
		}
		zend_hash_init( &LUASANDBOX_G(additional_libraries), 0, NULL, NULL, 1 );
		if ( LUASANDBOX_G(library_loaders).nNumOfElements ) {
			/* Clean up old values, if any. */
			zend_hash_destroy( &LUASANDBOX_G(library_loaders) );
		}
		zend_hash_init( &LUASANDBOX_G(library_loaders), 0, NULL, NULL, 1 );
		
		/* Cast new value from php.ini or ini_set() to *char, if necessary. */
		PCRE2_SPTR list;
#if PHP_VERSION_ID < 70000
		list = (PCRE2_SPTR)new_value;
#else
		list = (PCRE2_SPTR)ZSTR_VAL(new_value);
#endif
		PCRE2_SIZE list_len = (PCRE2_SIZE)strlen( (char *)list );

		PCRE2_SPTR pattern = "(?<lib>[^:=\\s]+)\\s*=\\s*(?<path>[^:\\s]+)";
		int errornumber;
		PCRE2_SIZE erroroffset;
		pcre2_code * re = pcre2_compile(
			pattern,
			PCRE2_ZERO_TERMINATED,
			PCRE2_NEVER_UTF,
			&errornumber,
			&erroroffset,
			NULL
		);
		pcre2_match_data * matches = pcre2_match_data_create_from_pattern( re, NULL );
		PCRE2_SIZE offset = 0;
		while ( offset < list_len && pcre2_match(re, list, list_len, offset, 0, matches, NULL) > 0 ) {
			offset = pcre2_get_ovector_pointer( matches )[1];
			PCRE2_UCHAR * lib;
			PCRE2_SIZE lib_len;
			pcre2_substring_get_byname( matches, "lib", &lib, &lib_len );
			PCRE2_UCHAR * path;
			PCRE2_SIZE path_len;
			pcre2_substring_get_byname( matches, "path", &path, &path_len );
			if ( lib_len && path_len ) {
				void * lib_so = dlopen( path, RTLD_LAZY | RTLD_GLOBAL );
				if ( lib_so ) {
					zval lib_so_z;
					ZVAL_PTR(&lib_so_z, lib_so);
					char loader[32] = "luaopen_";
					strcat( loader, lib );
					char * error = dlerror();
					if ( !error ) {					
						lua_CFunction loader_ptr = (lua_CFunction) dlsym( lib_so, loader );
						error = dlerror();
						if ( !error && loader_ptr ) {
							zval loader_z;
							ZVAL_PTR(&loader_z, loader_ptr);
							zend_hash_str_add_new( &LUASANDBOX_G(library_loaders), lib, lib_len, &loader_z );
							zval path_z;
							ZVAL_STR(&path_z, zend_string_init(path, path_len, 1));
							zend_hash_str_add_new( &LUASANDBOX_G(additional_libraries), lib, lib_len, &lib_so_z );
						} else {
							php_printf( "Could not load symbol %s from library %s: error %s\n", loader, path, error );
							php_error_docref(
								NULL TSRMLS_CC, E_ERROR, "Could not load symbol %s from library %s: error %s\n",
								loader, path, error
							);
							return FAILURE;
						}
					}
				}
			}
			pcre2_substring_free( lib );
			pcre2_substring_free( path );
		}
		pcre2_match_data_free( matches );
		pcre2_code_free( re );
		return SUCCESS;
	}
	return FAILURE;
}
/* }}} */

PHP_INI_BEGIN()
/**
 * Allowed global variables. Omissions are:
 *   * pcall, xpcall: We have our own versions which don't allow interception of
 *     timeout etc. errors.
 *   * loadfile: insecure.
 *   * load, loadstring: Probably creates a protected environment so has
 *     the same problem as pcall. Also omitting these makes analysis of the
 *     code for runtime etc. feasible.
 *   * print: Not compatible with a sandbox environment
 *   * tostring: Provides addresses of tables and functions, which provides an
 *     easy ASLR workaround or heap address discovery mechanism for a memory
 *     corruption exploit. We have our own version.
 *   * Any new or undocumented functions like newproxy.
 *   * package: cpath, loadlib etc. are insecure.
 *   * coroutine: Not useful for our application so unreviewed at present.
 *   * io, file, os: insecure
 *   * debug: Provides various ways to break the sandbox, such as setupvalue()
 *     and getregistry().
 */
	PHP_INI_ENTRY(
		PHP_LUASANDBOX_INI_ALLOWED_GLOBALS,	"assert,error,getfenv,getmetatable,ipairs,next,pairs,rawequal,rawget,rawset,select,setfenv,setmetatable,tonumber,type,unpack,_G,_VERSION,string,table,math,os,debug",
		PHP_INI_SYSTEM,
		luasandbox_update_allowed_globals
	)
	PHP_INI_ENTRY(
		PHP_LUASANDBOX_INI_ADDITIONAL_LIBRARIES,
		"",
		PHP_INI_SYSTEM,
		luasandbox_update_additional_libraries
	)
PHP_INI_END()

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(luasandbox)
{
	/* php.ini settings. */
	REGISTER_INI_ENTRIES();

	zend_class_entry ce;
	INIT_CLASS_ENTRY(ce, "LuaSandbox", luasandbox_methods);
	luasandbox_ce = zend_register_internal_class(&ce TSRMLS_CC);

	luasandbox_ce->create_object = luasandbox_new;
	zend_declare_class_constant_long(luasandbox_ce,
		"SAMPLES", sizeof("SAMPLES")-1, LUASANDBOX_SAMPLES TSRMLS_CC);
	zend_declare_class_constant_long(luasandbox_ce,
		"SECONDS", sizeof("SECONDS")-1, LUASANDBOX_SECONDS TSRMLS_CC);
	zend_declare_class_constant_long(luasandbox_ce,
		"PERCENT", sizeof("PERCENT")-1, LUASANDBOX_PERCENT TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "LuaSandboxError", luasandbox_empty_methods);
	luasandboxerror_ce = compat_zend_register_internal_class_ex(&ce, zend_exception_get_default(TSRMLS_C));
	zend_declare_class_constant_long(luasandboxerror_ce,
		"RUN", sizeof("RUN")-1, LUA_ERRRUN TSRMLS_CC);
	zend_declare_class_constant_long(luasandboxerror_ce,
		"SYNTAX", sizeof("SYNTAX")-1, LUA_ERRSYNTAX TSRMLS_CC);
	zend_declare_class_constant_long(luasandboxerror_ce,
		"MEM", sizeof("MEM")-1, LUA_ERRMEM TSRMLS_CC);
	zend_declare_class_constant_long(luasandboxerror_ce,
		"ERR", sizeof("ERR")-1, LUA_ERRERR TSRMLS_CC);

	INIT_CLASS_ENTRY(ce, "LuaSandboxRuntimeError", luasandbox_empty_methods);
	luasandboxruntimeerror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxerror_ce);

	INIT_CLASS_ENTRY(ce, "LuaSandboxFatalError", luasandbox_empty_methods);
	luasandboxfatalerror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxerror_ce);

	INIT_CLASS_ENTRY(ce, "LuaSandboxSyntaxError", luasandbox_empty_methods);
	luasandboxsyntaxerror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxfatalerror_ce);

	INIT_CLASS_ENTRY(ce, "LuaSandboxMemoryError", luasandbox_empty_methods);
	luasandboxmemoryerror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxfatalerror_ce);

	INIT_CLASS_ENTRY(ce, "LuaSandboxErrorError", luasandbox_empty_methods);
	luasandboxerrorerror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxfatalerror_ce);

	INIT_CLASS_ENTRY(ce, "LuaSandboxTimeoutError", luasandbox_empty_methods);
	luasandboxtimeouterror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxfatalerror_ce);

	// Deprecated, for catch blocks only
	INIT_CLASS_ENTRY(ce, "LuaSandboxEmergencyTimeoutError", luasandbox_empty_methods);
	luasandboxemergencytimeouterror_ce = compat_zend_register_internal_class_ex(&ce, luasandboxfatalerror_ce);

	INIT_CLASS_ENTRY(ce, "LuaSandboxFunction", luasandboxfunction_methods);
	luasandboxfunction_ce = zend_register_internal_class(&ce TSRMLS_CC);
	luasandboxfunction_ce->create_object = luasandboxfunction_new;

#if PHP_VERSION_ID >= 70000
	memcpy(&luasandbox_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	luasandbox_object_handlers.offset = XtOffsetOf(php_luasandbox_obj, std);
	luasandbox_object_handlers.free_obj = (zend_object_free_obj_t)luasandbox_free_storage;
	memcpy(&luasandboxfunction_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	luasandboxfunction_object_handlers.offset = XtOffsetOf(php_luasandboxfunction_obj, std);
	luasandboxfunction_object_handlers.free_obj = (zend_object_free_obj_t)luasandboxfunction_free_storage;
#endif

	luasandbox_timer_minit(TSRMLS_C);

	return SUCCESS;
}
/* }}} */

/** {{{ luasandbox_init_globals */
static PHP_GINIT_FUNCTION(luasandbox)
{
	memset(luasandbox_globals, 0, sizeof(*luasandbox_globals));
}
/* }}} */

/** {{{ PHP_GSHUTDOWN_FUNCTION(luasandbox) */
static PHP_GSHUTDOWN_FUNCTION(luasandbox)
{
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(luasandbox)
{
	luasandbox_timer_mshutdown(TSRMLS_C);
	zend_hash_destroy( &LUASANDBOX_G(allowed_globals) );
	zend_string * lib_z;
	zval * lib_so_z;
	ZEND_HASH_FOREACH_STR_KEY_VAL(&LUASANDBOX_G(additional_libraries), lib_z, lib_so_z)
		dlclose( Z_PTR_P(lib_so_z) );
	ZEND_HASH_FOREACH_END();
	zend_hash_destroy( &LUASANDBOX_G(additional_libraries) );
	zend_hash_destroy( &LUASANDBOX_G(library_loaders) );
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(luasandbox)
{
#ifdef HHVM
	// Under HHVM, free_storage handlers are not called on request shutdown
	if (LUASANDBOX_G(active_count)) {
		php_error(E_WARNING, "leaking %ld LuaSandbox object(s)",
			LUASANDBOX_G(active_count));
		LUASANDBOX_G(active_count) = 0;
	}
#endif

	return SUCCESS;
}

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(luasandbox)
{
	php_info_print_table_start();
	php_info_print_table_row( 2, "luasandbox support", "enabled" );

	char libraries[255] = "";
	zend_string * lib_z;
	int counter = 0;
	ZEND_HASH_FOREACH_STR_KEY(&LUASANDBOX_G(additional_libraries), lib_z)
		strcat( libraries, ZSTR_VAL(lib_z) );
		if ( ++counter < LUASANDBOX_G(additional_libraries).nNumOfElements ) {
			strcat( libraries, ", " );
		}
	ZEND_HASH_FOREACH_END();
	php_info_print_table_row( 2, "Loaded additional libraries", libraries );

	char globals[1023] = "";
	zend_string * global_z;
	counter = 0;
	ZEND_HASH_FOREACH_STR_KEY(&LUASANDBOX_G(allowed_globals), global_z)
		strcat( globals, ZSTR_VAL(global_z) );
		if ( ++counter < LUASANDBOX_G(allowed_globals).nNumOfElements ) {
			strcat( globals, ", " );
		}
	ZEND_HASH_FOREACH_END();
	php_info_print_table_row( 2, "Allowed globals", globals );

	php_info_print_table_end();
}
/* }}} */

/** {{{ luasandbox_new
 *
 * "new" handler for the LuaSandbox class
 */
static object_constructor_ret_t luasandbox_new(zend_class_entry *ce TSRMLS_DC)
{
	php_luasandbox_obj * sandbox;

	// Create the internal object
#if PHP_VERSION_ID < 70000
	sandbox = (php_luasandbox_obj*)ecalloc(1, sizeof(php_luasandbox_obj));
#elif PHP_VERSION_ID < 70300
	sandbox = (php_luasandbox_obj*)ecalloc(1, sizeof(php_luasandbox_obj) + zend_object_properties_size(ce));
#else
	sandbox = (php_luasandbox_obj*)zend_object_alloc(sizeof(php_luasandbox_obj), ce);
#endif
	zend_object_std_init(&sandbox->std, ce TSRMLS_CC);
#if PHP_VERSION_ID > 50399
	object_properties_init(&sandbox->std, ce);
#endif
	sandbox->alloc.memory_limit = (size_t)-1;
	sandbox->allow_pause = 1;

	// Initialise the Lua state
	sandbox->state = luasandbox_newstate(sandbox TSRMLS_CC);

	// Initialise the timer
	luasandbox_timer_create(&sandbox->timer, sandbox);

#if PHP_VERSION_ID < 70000
	// Put the object into the store
	zend_object_value retval;
	retval.handle = zend_objects_store_put(
		sandbox,
		(zend_objects_store_dtor_t)zend_objects_destroy_object,
		(zend_objects_free_object_storage_t)luasandbox_free_storage,
		NULL TSRMLS_CC);
	retval.handlers = zend_get_std_object_handlers();
	LUASANDBOX_G(active_count)++;
	return retval;
#else
	sandbox->std.handlers = &luasandbox_object_handlers;

	LUASANDBOX_G(active_count)++;
	return &sandbox->std;
#endif
}
/* }}} */

/** {{{ luasandbox_newstate
 *
 * Create a new lua_State which is suitable for running sandboxed scripts in.
 * Initialise libraries and any necessary registry entries.
 */
static lua_State * luasandbox_newstate(php_luasandbox_obj * intern TSRMLS_DC)
{

	lua_State * L = luasandbox_alloc_new_state(&intern->alloc, intern);

	if (L == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR,
			"Attempt to allocate a new Lua state failed");
	}

	lua_atpanic(L, luasandbox_panic);

	// Register the standard library
	luasandbox_lib_register(L TSRMLS_CC);

	// Set up the data conversion module
	luasandbox_data_conversion_init(L);

	// Create a table for storing chunks
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");

	// Register a pointer to the PHP object so that C functions can find it
	lua_pushlightuserdata(L, (void*)intern);
	lua_setfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");

	return L;
}
/* }}} */

/** {{{ luasandbox_free_storage
 *
 * "Free storage" handler for LuaSandbox objects.
 */
static void luasandbox_free_storage(zend_object *object TSRMLS_DC)
{
	php_luasandbox_obj * sandbox = php_luasandbox_fetch_object(object);

	luasandbox_timer_destroy(&sandbox->timer);
	if (sandbox->state) {
		luasandbox_alloc_delete_state(&sandbox->alloc, sandbox->state);
		sandbox->state = NULL;
	}
	zend_object_std_dtor(&sandbox->std TSRMLS_CC);
#if PHP_VERSION_ID < 70000
	efree(object);
#endif
	LUASANDBOX_G(active_count)--;
}
/* }}} */

/** {{{ luasandboxfunction_new
 *
 * "new" handler for the LuaSandboxFunction class.
 */
static object_constructor_ret_t luasandboxfunction_new(zend_class_entry *ce TSRMLS_DC)
{
	php_luasandboxfunction_obj * intern;

	// Create the internal object
#if PHP_VERSION_ID < 70000
	intern = (php_luasandboxfunction_obj*)ecalloc(1, sizeof(php_luasandboxfunction_obj));
#elif PHP_VERSION_ID < 70300
	intern = (php_luasandboxfunction_obj*)ecalloc(1, sizeof(php_luasandboxfunction_obj) + zend_object_properties_size(ce));
#else
	intern = (php_luasandboxfunction_obj*)zend_object_alloc(sizeof(php_luasandboxfunction_obj), ce);
#endif

	zend_object_std_init(&intern->std, ce TSRMLS_CC);
#if PHP_VERSION_ID > 50399
	object_properties_init(&intern->std, ce);
#endif

#if PHP_VERSION_ID < 70000
	// Put the object into the store
	zend_object_value retval;
	retval.handle = zend_objects_store_put(
		intern,
		(zend_objects_store_dtor_t)zend_objects_destroy_object,
		(zend_objects_free_object_storage_t)luasandboxfunction_free_storage,
		NULL TSRMLS_CC);
	retval.handlers = zend_get_std_object_handlers();
	return retval;
#else
	intern->std.handlers = &luasandboxfunction_object_handlers;
	return &intern->std;
#endif
}
/* }}} */

/** {{{ luasandboxfunction_free_storage
 *
 * "Free storage" handler for LuaSandboxFunction objects. Deletes the chunk
 * from the registry and decrements the reference counter for the parent
 * LuaSandbox object.
 */
static void luasandboxfunction_free_storage(zend_object *object TSRMLS_DC)
{
	php_luasandboxfunction_obj * func = php_luasandboxfunction_fetch_object(object);
	if (LUASANDBOXFUNCTION_SANDBOX_IS_OK(func)) {
		zval *zsandbox = LUASANDBOXFUNCTION_GET_SANDBOX_ZVALPTR(func);
		php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(zsandbox);
		if (sandbox && sandbox->state) {
			lua_State * L = sandbox->state;

			// Delete the chunk
			if (func->index) {
				lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
				lua_pushnil(L);
				lua_rawseti(L, -2, func->index);
				lua_pop(L, 1);
			}
		}

		// Delete the parent reference
		zval_ptr_dtor(&func->sandbox);
#if PHP_VERSION_ID < 70000
		func->sandbox = NULL;
#else
		ZVAL_UNDEF(&func->sandbox);
#endif
	}
	zend_object_std_dtor(&func->std TSRMLS_CC);
#if PHP_VERSION_ID < 70000
	efree(object);
#endif
}
/* }}} */

/** {{{ luasandbox_panic
 *
 * The Lua panic function. It is necessary to raise an E_ERROR, and thus do a
 * longjmp(), since if this function returns, Lua will call exit(), breaking
 * the Apache child.
 *
 * Generally, a panic will happen if the Lua API is used incorrectly in an
 * unprotected environment. Currently this means C code which is called from
 * PHP, not directly or indirectly from lua_pcall(). Sandboxed Lua code is run
 * under lua_pcall() so can't cause a panic.
 *
 * Note that sandboxed Lua code may be executed in an unprotected environment
 * if C code accesses a variable with a metamethod defined by the sandboxed
 * code. For this reason, the "raw" functions such as lua_rawget() should be
 * used where this is a possibility.
 */
static int luasandbox_panic(lua_State * L)
{
	TSRMLS_FETCH();
	php_error_docref(NULL TSRMLS_CC, E_ERROR,
		"PANIC: unprotected error in call to Lua API (%s)",
		luasandbox_error_to_string(L, -1));
	return 0;
}
/* }}} */

/** {{{ luasandbox_state_from_zval
 *
 * Get a lua state from a zval* containing a LuaSandbox object. If the zval*
 * contains something else, bad things will happen.
 */
static lua_State * luasandbox_state_from_zval(zval * this_ptr TSRMLS_DC)
{
	php_luasandbox_obj * intern = GET_LUASANDBOX_OBJ(this_ptr);
	return intern->state;
}
/* }}} */

/** {{{ luasandbox_load_helper
 *
 * Common code for LuaSandbox::loadString() and LuaSandbox::loadBinary(). The
 * "binary" parameter will be 1 for loadBinary() and 0 for loadString().
 *
 * For catching Lua errors that might be raised, we need a struct and a helper
 * function too.
 */

struct luasandbox_load_helper_params {
	php_luasandbox_obj * sandbox;
	zval *zthis;
	zval *return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls;
#endif
	char *code;
	char *chunkName;
	str_param_len_t codeLength;
};

static int luasandbox_load_helper_protected(lua_State* L) {
	struct luasandbox_load_helper_params *p = (struct luasandbox_load_helper_params *)lua_touserdata(L, 1);
	int status;
	zval *return_value = p->return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls = p->tsrm_ls;
#endif

	// Parse the string into a function on the stack
	status = luaL_loadbuffer(L, p->code, p->codeLength, p->chunkName);

	// Handle any error from luaL_loadbuffer
	if (status != 0) {
		luasandbox_handle_error(p->sandbox, status TSRMLS_CC);
		RETVAL_FALSE;
		return 0;
	}

	// Make a zval out of it, and return false on error
	if (!luasandbox_lua_to_zval(p->return_value, L, lua_gettop(L), p->zthis, NULL TSRMLS_CC) ||
		Z_TYPE_P(p->return_value) == IS_NULL
	) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "too many chunks loaded already");
		RETVAL_FALSE;
	}

	// Balance the stack
	lua_pop(L, 1);
	return 0;
}

static void luasandbox_load_helper(int binary, INTERNAL_FUNCTION_PARAMETERS)
{
	struct luasandbox_load_helper_params p;
	str_param_len_t chunkNameLength;
	lua_State * L;
	int have_mark;
	int was_paused;
	int status;

	p.sandbox = GET_LUASANDBOX_OBJ(getThis());
	L = p.sandbox->state;
	CHECK_VALID_STATE(L);

	p.chunkName = NULL;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|s",
				&p.code, &p.codeLength, &p.chunkName, &chunkNameLength) == FAILURE) {
		RETURN_FALSE;
	}

	if (p.chunkName == NULL) {
		p.chunkName = "";
	} else {
		// Check chunkName for nulls
		if (strlen(p.chunkName) != chunkNameLength) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"chunk name may not contain null characters");
			RETURN_FALSE;
		}
	}

	// Check to see if the code is binary (with precompiled data mark) if this
	// was called as loadBinary(), and plain code (without mark) if this was
	// called as loadString()
	have_mark = (php_memnstr(p.code, LUA_SIGNATURE,
		sizeof(LUA_SIGNATURE) - 1, p.code + p.codeLength) != NULL);
	if (binary && !have_mark) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"the string does not appear to be a valid binary chunk");
		RETURN_FALSE;
	} else if (!binary && have_mark) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"cannot load code with a Lua binary chunk marker escape sequence in it");
		RETURN_FALSE;
	}

	// Make sure this is counted against the Lua usage time limit
	was_paused = luasandbox_timer_is_paused(&p.sandbox->timer);
	luasandbox_timer_unpause(&p.sandbox->timer);

	p.zthis = getThis();
#if defined(ZTS) && PHP_VERSION_ID < 70000
	p.tsrm_ls = tsrm_ls;
#endif
	p.return_value = return_value;
	status = lua_cpcall(L, luasandbox_load_helper_protected, &p);

	// If the timers were paused before, re-pause them now
	if (was_paused) {
		luasandbox_timer_pause(&p.sandbox->timer);
	}

	// Handle any error from Lua
	if (status != 0) {
		luasandbox_handle_error(p.sandbox, status TSRMLS_CC);
		RETURN_FALSE;
	}
}
/* }}} */

/** {{{ proto static array LuaSandbox::getVersionInfo()
 *
 * Return the versions of LuaSandbox and Lua, as an associative array.
 */
PHP_METHOD(LuaSandbox, getVersionInfo)
{
	array_init_size(return_value, 2);
	compat_add_assoc_string(return_value, "LuaSandbox", LUASANDBOX_VERSION);
	compat_add_assoc_string(return_value, "Lua", LUA_RELEASE);
}

/* }}} */

/** {{{ proto LuaSandboxFunction LuaSandbox::loadString(string code, string chunkName = '')
 *
 * Load a string into the LuaSandbox object. Returns a LuaSandboxFunction object
 * which can be called or dumped.
 *
 * Note that global functions and variables defined in the chunk will not be
 * present in the Lua state until the chunk is executed. Function definitions
 * in Lua are a kind of executable statement.
 */
PHP_METHOD(LuaSandbox, loadString)
{
	luasandbox_load_helper(0, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

/* }}} */

/** {{{ proto LuaSandboxFunction LuaSandbox::loadBinary(string bin, string chunkName = '')
 * Load a string containing a precompiled binary chunk from
 * LuaSandboxFunction::dump() or the Lua compiler luac. Returns a
 * LuaSandboxFunction object.
 */
PHP_METHOD(LuaSandbox, loadBinary)
{
	luasandbox_load_helper(1, INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
/* }}} */

/** {{{ luasandbox_handle_error
 *
 * Handles the error return situation from lua_pcall() and lua_load(), where a
 * status is returned and an error message pushed to the stack. Throws a suitable
 * exception.
 *
 * This method shouldn't raise any Lua errors. This is accomplished by
 * judicious disabling of the memory limit and by using a pcall for one bit
 * that is otherwise unsafe.
 */

static int luasandbox_safe_trace_to_zval(lua_State* L) {
	zval *zsandbox = (zval *)lua_touserdata(L, 2);
	zval *ztrace = (zval *)lua_touserdata(L, 3);
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls = (void ***)lua_touserdata(L, 4);
#endif

	luasandbox_lua_to_zval(ztrace, L, 1, zsandbox, NULL TSRMLS_CC);
	return 0;
}

static void luasandbox_handle_error(php_luasandbox_obj * sandbox, int status TSRMLS_DC)
{
	lua_State * L = sandbox->state;
	const char * errorMsg;
	zend_class_entry * ce;
	zval *zex, *ztrace;
	size_t old_memory_limit;

#if PHP_VERSION_ID < 70000
	MAKE_STD_ZVAL(zex);
	ALLOC_INIT_ZVAL(ztrace); // IS_NULL if lua_to_zval fails.
#else
	zval zvex, zvtrace;
	zex = &zvex;
	ztrace = &zvtrace;
	ZVAL_NULL(ztrace);
#endif

	if (EG(exception)) {
		lua_pop(L, 1);
		return;
	}

	// Temporarily disable the memory limit while fetching the error string, so
	// the fetch doesn't raise a memory error.
	old_memory_limit = sandbox->alloc.memory_limit;
	sandbox->alloc.memory_limit = (size_t)-1;
	errorMsg = luasandbox_error_to_string(L, -1);
	sandbox->alloc.memory_limit = old_memory_limit;

	switch (status) {
		case LUA_ERRRUN:
		default:
			if (luasandbox_is_fatal(L, -1)) {
				if (!strcmp(errorMsg, luasandbox_timeout_message)) {
					ce = luasandboxtimeouterror_ce;
				} else {
					ce = luasandboxfatalerror_ce;
				}
			} else {
				ce = luasandboxruntimeerror_ce;
			}
			break;
		case LUA_ERRSYNTAX:
			ce = luasandboxsyntaxerror_ce;
			break;
		case LUA_ERRMEM:
			ce = luasandboxmemoryerror_ce;
			break;
		case LUA_ERRERR:
			ce = luasandboxerrorerror_ce;
			break;
	}

	object_init_ex(zex, ce);

	if (luasandbox_is_trace_error(L, -1)) {
		// Here we can't just ignore the memory limit since
		// luasandbox_lua_to_zval can throw non-memory errors. So make a pcall
		// to do the conversion.
		//
		// But we can and should ignore the memory limit while pushing the
		// parameters for the pcall onto the stack.

		old_memory_limit = sandbox->alloc.memory_limit;
		sandbox->alloc.memory_limit = (size_t)-1;
		lua_pushcfunction(L, luasandbox_safe_trace_to_zval);
		lua_rawgeti(L, -2, 3);
		lua_pushlightuserdata(L, LUASANDBOX_GET_CURRENT_ZVAL_PTR(sandbox));
		lua_pushlightuserdata(L, ztrace);
		lua_pushlightuserdata(L,
#if defined(ZTS) && PHP_VERSION_ID < 70000
			tsrm_ls
#else
			NULL
#endif
		);
		sandbox->alloc.memory_limit = old_memory_limit;
		if (lua_pcall(L, 4, 0, 0) == 0) {
			// Put it in the exception object
			zend_update_property(ce, zex, "luaTrace", sizeof("luaTrace")-1, ztrace TSRMLS_CC);
		} else {
			// lua_pcall pushed an error on the stack.
			old_memory_limit = sandbox->alloc.memory_limit;
			sandbox->alloc.memory_limit = (size_t)-1;
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Failed to generate Lua trace (%s)",
				luasandbox_error_to_string(L, -1));
			sandbox->alloc.memory_limit = old_memory_limit;
			lua_pop(L, 1);
		}
	}
#if PHP_VERSION_ID < 70000
	zval_ptr_dtor(&ztrace);
#else
	zval_ptr_dtor(&zvtrace);
#endif

	// Initialise standard properties
	// We would get Zend to do this, but the code for it is wrapped inside some
	// very inconvenient interfaces (so inconvenient that Zend itself
	// duplicates this code in several places).
	zend_update_property_string(ce, zex, "message", sizeof("message")-1, errorMsg TSRMLS_CC);
	zend_update_property_long(ce, zex, "code", sizeof("code")-1, status TSRMLS_CC);

	zend_throw_exception_object(zex TSRMLS_CC);
	lua_pop(L, 1);
}
/* }}} */

/** {{{ proto void LuaSandbox::setMemoryLimit(int limit)
 *
 * Set the memory limit for the Lua state. If the memory limit is exceeded,
 * the allocator will return NULL. When running protected code, this will
 * result in a LuaSandboxError exception being thrown. If this occurs in
 * unprotected code, say due to loading too many functions with loadString(),
 * a panic will be triggered, leading to a PHP fatal error.
 */
PHP_METHOD(LuaSandbox, setMemoryLimit)
{
	long_param_t limit;
	php_luasandbox_obj * intern = GET_LUASANDBOX_OBJ(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l",
				&limit) == FAILURE)
	{
		RETURN_FALSE;
	}

	intern->alloc.memory_limit = limit;
}
/* }}} */


/** {{{ proto void LuaSandbox::setCPULimit(mixed limit)
 *
 * Set the limit of CPU time (user+system) for all LuaSandboxFunction::call()
 * calls against this LuaSandbox instance. The limit is specified in seconds,
 * or false to disable the limiter.
 *
 * When the time limit expires, a flag will be set which causes a
 * LuaSandboxError exception to be thrown when the currently-running Lua
 * statement finishes.
 *
 * Setting the time limit from a callback while Lua is running causes the timer
 * to be reset, or started if it was not already running.
 */
PHP_METHOD(LuaSandbox, setCPULimit)
{
	zval *zp_limit = NULL;

	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());

	struct timespec limit = {0, 0};

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z/",
		&zp_limit) == FAILURE)
	{
		RETURN_FALSE;
	}

	if (!zp_limit
#ifdef IS_BOOL
		|| (Z_TYPE_P(zp_limit) == IS_BOOL && Z_BVAL_P(zp_limit) == 0)
#else
		|| Z_TYPE_P(zp_limit) == IS_FALSE
#endif
	) {
		// No limit
		sandbox->is_cpu_limited = 0;
	} else {
		convert_to_double(zp_limit);
		luasandbox_set_timespec(&limit, Z_DVAL_P(zp_limit));
		sandbox->is_cpu_limited = 1;
	}

	// Set the timer
	luasandbox_timer_set_limit(&sandbox->timer, &limit);
}
/* }}} */

/** {{{ luasandbox_set_timespec
 * Initialise a timespec structure with the time in seconds given by the source
 * argument.
 */
static void luasandbox_set_timespec(struct timespec * dest, double source)
{
	double fractional, integral;
	if (source < 0) {
		dest->tv_sec = dest->tv_nsec = 0;
		return;
	}

	fractional = modf(source, &integral);
	dest->tv_sec = (time_t)integral;
	dest->tv_nsec = (long)(fractional * 1000000000.0);
	if (dest->tv_nsec >= 1000000000L) {
		dest->tv_nsec -= 1000000000L;
		dest->tv_sec ++;
	}
}

/* }}} */

/** {{{ proto float LuaSandbox::getCPUUsage()
 *
 * Get the amount of CPU used by this LuaSandbox instance, including any PHP
 * functions called by Lua.
 */
PHP_METHOD(LuaSandbox, getCPUUsage)
{
	struct timespec ts;
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		RETURN_FALSE;
	}

	luasandbox_timer_get_usage(&sandbox->timer, &ts);
	RETURN_DOUBLE(ts.tv_sec + 1e-9 * ts.tv_nsec);
}
/* }}} */

/** {{{ proto bool LuaSandbox::pauseUsageTimer()
 *
 * Pause the CPU usage timer, and the time limit set by LuaSandbox::setCPULimit.
 *
 * This only has effect when called from within a callback from Lua. When
 * execution returns to Lua, the timers will be automatically unpaused. If a
 * new call into Lua is made, the timers will be unpaused for the duration of
 * that call.
 *
 * If a PHP callback calls into Lua again with timers not paused, and then that
 * Lua function calls into PHP again, the second PHP call will not be able to
 * pause the timers. The logic is that even though the second PHP call would
 * avoid counting the CPU usage against the limit, the first call still counts
 * it.
 *
 * Returns true if the timers are now paused, false if not.
 */
PHP_METHOD(LuaSandbox, pauseUsageTimer)
{
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		RETURN_FALSE;
	}

	if ( !sandbox->allow_pause || !sandbox->in_lua ) {
		RETURN_FALSE;
	}

	luasandbox_timer_pause(&sandbox->timer);
	RETURN_TRUE;
}
/* }}} */

/** {{{ proto void LuaSandbox::unpauseUsageTimer()
 *
 * Unpause timers paused by LuaSandbox::pauseUsageTimer.
 */
PHP_METHOD(LuaSandbox, unpauseUsageTimer)
{
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		RETURN_FALSE;
	}

	luasandbox_timer_unpause(&sandbox->timer);
	RETURN_NULL();
}
/* }}} */

/* {{{ proto boolean LuaSandbox::enableProfiler(float period = 0.002)
 *
 * Enable the profiler. Profiling will begin when Lua code is entered.
 *
 * We use a sampling profiler, with samples taken with the specified sampling
 * period, in seconds. Testing indicates that at least on Linux, setting a
 * period less than 1ms will lead to a high overrun count but no performance
 * problems.
 */
PHP_METHOD(LuaSandbox, enableProfiler)
{
	double period = 2e-3;
	struct timespec ts;
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|d", &period) == FAILURE) {
		RETURN_FALSE;
	}

	luasandbox_set_timespec(&ts, period);
	RETURN_BOOL(luasandbox_timer_enable_profiler(&sandbox->timer, &ts));
}
/* }}} */

/* {{{ proto void LuaSandbox::disableProfiler()
 *
 * Disable the profiler.
 */
PHP_METHOD(LuaSandbox, disableProfiler)
{
	struct timespec ts = {0, 0};
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());
	luasandbox_timer_enable_profiler(&sandbox->timer, &ts);
}
/* }}} */

#ifndef HHVM
static int luasandbox_sort_profile(const void *a, const void *b TSRMLS_DC) /* {{{ */
{
	Bucket *bucket_a, *bucket_b;
	size_t value_a, value_b;

#if PHP_VERSION_ID < 70000
	bucket_a = *((Bucket **) a);
	bucket_b = *((Bucket **) b);
	value_a = *(size_t*)bucket_a->pData;
	value_b = *(size_t*)bucket_b->pData;
#else
	bucket_a = (Bucket *) a;
	bucket_b = (Bucket *) b;
	value_a = (size_t)Z_LVAL(bucket_a->val);
	value_b = (size_t)Z_LVAL(bucket_b->val);
#endif

	if (value_a < value_b) {
		return 1;
	} else if (value_a > value_b) {
		return -1;
	} else {
		return 0;
	}
}

/* }}} */
#endif

/* {{{ proto array LuaSandbox::getProfilerFunctionReport(int what = LuaSandbox::SECONDS)
 *
 * For a profiling instance previously started by enableProfiler(), get a report
 * of the cost of each function. The return value will be an associative array
 * mapping the function name (with source file and line defined in angle
 * brackets) to the cost.
 *
 * The measurement unit used for the cost is determined by the what parameter:
 *   - LuaSandbox::SAMPLES: Measure in number of samples
 *   - LuaSandbox::SECONDS: Measure in seconds of CPU time (default)
 *   - LuaSandbox::PERCENT: Measure percentage of CPU time
 */
#ifdef HHVM
// We need to wrap this method in ext_luasandbox.php for HHVM
PHP_METHOD(LuaSandbox, _internal_getProfilerFunctionReport)
#else
PHP_METHOD(LuaSandbox, getProfilerFunctionReport)
#endif
{
	long_param_t units = LUASANDBOX_SECONDS;
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &units) == FAILURE) {
		RETURN_FALSE;
	}

	if (units != LUASANDBOX_SECONDS
			&& units != LUASANDBOX_SAMPLES
			&& units != LUASANDBOX_PERCENT )
	{
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"invalid value for \"units\" passed to LuaSandbox::getProfilerFunctionReport");
		RETURN_FALSE;
	}

	HashTable * counts = sandbox->timer.function_counts;
	if (!counts) {
		array_init(return_value);
		return;
	}

	// Sort the input array in descending order of time usage
#ifdef HHVM
	// HHVM's zend_hash_sort ignores the compar argument. The sorting is done
	// in the systemlib instead.
#elif PHP_VERSION_ID < 70000
	zend_hash_sort(counts, zend_qsort, luasandbox_sort_profile, 0 TSRMLS_CC);
#else
	zend_hash_sort_ex(counts, zend_qsort, luasandbox_sort_profile, 0);
#endif

	array_init_size(return_value, zend_hash_num_elements(counts));

	// Copy the data to the output array, scaling as necessary
	double scale = 0.;
	if (units == LUASANDBOX_SECONDS) {
		struct timespec * ts = &sandbox->timer.profiler_period;
		scale = ts->tv_sec + ts->tv_nsec * 1e-9;
	} else if (units == LUASANDBOX_PERCENT) {
		if (sandbox->timer.total_count != 0) {
			scale = 100. / sandbox->timer.total_count;
		}
	}

#if PHP_VERSION_ID < 70000
	HashPosition p;
	for (zend_hash_internal_pointer_reset_ex(counts, &p);
			zend_hash_get_current_key_type_ex(counts, &p) != HASH_KEY_NON_EXISTANT;
			zend_hash_move_forward_ex(counts, &p))
	{
		size_t *count;
		char * func_name = "";
		uint func_name_length = 0;
		ulong lkey;

		zend_hash_get_current_key_ex(counts, &func_name, &func_name_length,
				&lkey, 0, &p);
		zend_hash_get_current_data_ex(counts, (void**)&count, &p);
		if (units == LUASANDBOX_SAMPLES) {
			add_assoc_long_ex(return_value, func_name, func_name_length, *count);
		} else {
			add_assoc_double_ex(return_value, func_name, func_name_length, *count * scale);
		}
	}
#else
	zend_string *key;
	zval *count, v;
	ZVAL_NULL(&v);
	ZEND_HASH_FOREACH_STR_KEY_VAL(counts, key, count)
	{
		if (units == LUASANDBOX_SAMPLES) {
			zend_hash_add(Z_ARRVAL_P(return_value), key, count);
		} else {
			ZVAL_DOUBLE(&v, Z_LVAL_P(count) * scale);
			zend_hash_add(Z_ARRVAL_P(return_value), key, &v);
		}
	} ZEND_HASH_FOREACH_END();
#endif

#ifdef LUASANDBOX_REPORT_OVERRUNS
	if (units == LUASANDBOX_SAMPLES) {
		add_assoc_long(return_value, "overrun", sandbox->timer.overrun_count);
	} else {
		add_assoc_double(return_value, "overrun", sandbox->timer.overrun_count * scale);
	}
#endif
}

/* }}} */

/** {{{ LuaSandbox::getMemoryUsage */
PHP_METHOD(LuaSandbox, getMemoryUsage)
{
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		RETURN_FALSE;
	}

	RETURN_LONG(sandbox->alloc.memory_usage);
}
/* }}} */

/** {{{ LuaSandbox::getPeakMemoryUsage */
PHP_METHOD(LuaSandbox, getPeakMemoryUsage)
{
	php_luasandbox_obj * sandbox = GET_LUASANDBOX_OBJ(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		RETURN_FALSE;
	}

	RETURN_LONG(sandbox->alloc.peak_memory_usage);
}
/* }}} */

/** {{{ proto array LuaSandbox::callFunction(string name, ...$args )
 *
 * Call a function in the global variable with the given name. The name may
 * contain "." characters, in which case the function is located via recursive
 * table accesses, as if the name were a Lua expression.
 *
 * If the variable does not exist, or is not a function, false will be returned
 * and a warning issued.
 *
 * Any arguments specified after the name will be passed through as arguments
 * to the function.
 *
 * For more information about calling Lua functions and the return values, see
 * LuaSandboxFunction::call().
 *
 * For catching Lua errors that might be raised, we need a struct and a helper
 * function too.
 */

struct LuaSandbox_callFunction_params {
	php_luasandbox_obj * sandbox;
	zval *zthis;
	zval *return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls;
#endif
	char *name;
	str_param_len_t nameLength;
	int numArgs;
	star_param_t args;
};

static int LuaSandbox_callFunction_protected(lua_State* L) {
	struct LuaSandbox_callFunction_params *p = (struct LuaSandbox_callFunction_params *)lua_touserdata(L, 1);
	zval *return_value = p->return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls = p->tsrm_ls;
#endif

	// Find the function
	if (!luasandbox_find_field(L, LUA_GLOBALSINDEX, p->name, p->nameLength)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"The specified lua function does not exist");
		RETVAL_FALSE;
	} else {
		// Call it
		luasandbox_call_helper(L, p->zthis, p->sandbox, p->args, p->numArgs, return_value TSRMLS_CC);
	}

	return 0;
}

PHP_METHOD(LuaSandbox, callFunction)
{
	struct LuaSandbox_callFunction_params p;
	int status;

	p.nameLength = 0;
	p.numArgs = 0;
	p.args = NULL;

	p.sandbox = GET_LUASANDBOX_OBJ(getThis());
	lua_State * L = p.sandbox->state;
	CHECK_VALID_STATE(L);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s*",
		&p.name, &p.nameLength, &p.args, &p.numArgs) == FAILURE)
	{
		RETURN_FALSE;
	}

	p.zthis = getThis();
	p.return_value = return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	p.tsrm_ls = tsrm_ls;
#endif
	status = lua_cpcall(L, LuaSandbox_callFunction_protected, &p);

	// Handle any error from Lua
	if (status != 0) {
		luasandbox_handle_error(p.sandbox, status TSRMLS_CC);
		RETVAL_FALSE;
	}

#if PHP_VERSION_ID < 70000
	// Delete varargs
	if (p.numArgs) {
		efree(p.args);
	}
#endif
}
/* }}} */

/** {{{ proto LuaSandboxFunction LuaSandbox::wrapPhpFunction(callable function)
 *
 * Wrap a PHP callable in a LuaSandboxFunction, so it can be passed into Lua as
 * an anonymous function.
 *
 * For more information about calling Lua functions and the return values, see
 * LuaSandboxFunction::call().
 *
 * For catching Lua errors that might be raised, we need a struct and a helper
 * function too.
 */

struct LuaSandbox_wrapPhpFunction_params {
	zval *zthis;
	zval *return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls;
#endif
	zval *z;
};

static int LuaSandbox_wrapPhpFunction_protected(lua_State* L) {
	struct LuaSandbox_wrapPhpFunction_params *p = (struct LuaSandbox_wrapPhpFunction_params *)lua_touserdata(L, 1);
	zval *return_value = p->return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls = p->tsrm_ls;
#endif

	luasandbox_push_zval_userdata(L, p->z);
	lua_pushcclosure(L, luasandbox_call_php, 1);

	if (!luasandbox_lua_to_zval(return_value, L, lua_gettop(L), p->zthis, NULL TSRMLS_CC) ||
		Z_TYPE_P(return_value) == IS_NULL
	) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"too many chunks loaded already");
		RETVAL_FALSE;
	}

	lua_pop(L, 1);
	return 0;
}

PHP_METHOD(LuaSandbox, wrapPhpFunction)
{
	struct LuaSandbox_wrapPhpFunction_params p;
	php_luasandbox_obj * sandbox;
	lua_State * L;
	int status;

	p.zthis = getThis();
	sandbox = GET_LUASANDBOX_OBJ(p.zthis);
	L = sandbox->state;
	CHECK_VALID_STATE(L);

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z",
		&p.z) == FAILURE)
	{
		RETVAL_FALSE;
	}

	p.return_value = return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	p.tsrm_ls = tsrm_ls;
#endif
	status = lua_cpcall(L, LuaSandbox_wrapPhpFunction_protected, &p);

	// Handle any error from Lua
	if (status != 0) {
		luasandbox_handle_error(sandbox, status TSRMLS_CC);
		RETVAL_FALSE;
	}
}
/* }}} */

/** {{{ luasandbox_function_init
 *
 * Common initialisation for LuaSandboxFunction methods. Initialise the
 * function, state and sandbox pointers.
 */
static int luasandbox_function_init(zval * this_ptr, php_luasandboxfunction_obj ** pfunc,
	lua_State ** pstate, php_luasandbox_obj ** psandbox TSRMLS_DC)
{
	*pfunc = GET_LUASANDBOXFUNCTION_OBJ(this_ptr);
	if (!*pfunc || !LUASANDBOXFUNCTION_SANDBOX_IS_OK(*pfunc) || !(*pfunc)->index) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"attempt to call uninitialized LuaSandboxFunction object" );
		return 0;
	}

	zval *zsandbox = LUASANDBOXFUNCTION_GET_SANDBOX_ZVALPTR(*pfunc);
	*psandbox = GET_LUASANDBOX_OBJ(zsandbox);
	*pstate = (*psandbox)->state;

	if (!*pstate) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid LuaSandbox state");
		return 0;
	}

	return 1;
}
/* }}} */

/** {{{ luasandbox_function_push
 *
 * Common initialisation for LuaSandboxFunction methods. Push the function onto
 * the Lua stack.
 */
static void luasandbox_function_push(php_luasandboxfunction_obj * pfunc, lua_State * pstate)
{
	// Find the function
	lua_getfield(pstate, LUA_REGISTRYINDEX, "php_luasandbox_chunks");
	lua_rawgeti(pstate, -1, pfunc->index);

	// Remove the table from the stack
	lua_remove(pstate, -2);
}
/* }}} */

/* {{{ proto private final LuaSandboxFunction::__construct()
 *
 * Construct a LuaSandboxFunction object. Do not call this directly, use
 * LuaSandbox::loadString().
 */
PHP_METHOD(LuaSandboxFunction, __construct)
{
	php_error_docref(NULL TSRMLS_CC, E_ERROR, "LuaSandboxFunction cannot be constructed directly");
}
/* }}} */

/** {{{ proto array LuaSandboxFunction::call( ...$args )
 *
 * Call a LuaSandboxFunction. The arguments to this function are passed through
 * to Lua.
 *
 * Errors considered to be the fault of the PHP code will result in the
 * function returning false and E_WARNING being raised, for example, a
 * resource type being used as an argument. Lua errors will result in a
 * LuaSandboxError exception being thrown.
 *
 * Lua functions inherently return a list of results. So on success, this
 * function returns an array containing all of the values returned by Lua,
 * with integer keys starting from zero. Lua may return no results, in which
 * case an empty array is returned.
 *
 * For catching Lua errors that might be raised, we need a struct and a helper
 * function too.
 */

struct LuaSandboxFunction_call_params {
	php_luasandbox_obj * sandbox;
	zval *return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls;
#endif
	php_luasandboxfunction_obj *func;
	int numArgs;
	star_param_t args;
};

static int LuaSandboxFunction_call_protected(lua_State* L) {
	struct LuaSandboxFunction_call_params *p = (struct LuaSandboxFunction_call_params *)lua_touserdata(L, 1);
	zval *return_value = p->return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	void ***tsrm_ls = p->tsrm_ls;
#endif

	luasandbox_function_push(p->func, L);
	luasandbox_call_helper(L, LUASANDBOXFUNCTION_GET_SANDBOX_ZVALPTR(p->func),
			p->sandbox, p->args, p->numArgs, return_value TSRMLS_CC);

	return 0;
}

PHP_METHOD(LuaSandboxFunction, call)
{
	struct LuaSandboxFunction_call_params p;
	lua_State * L;
	int status;

	p.return_value = return_value;
#if defined(ZTS) && PHP_VERSION_ID < 70000
	p.tsrm_ls = tsrm_ls;
#endif
	p.numArgs = 0;
	p.args = NULL;

	if (!luasandbox_function_init(getThis(), &p.func, &L, &p.sandbox TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "*",
		&p.args, &p.numArgs) == FAILURE)
	{
		RETURN_FALSE;
	}

	// Call the function
	status = lua_cpcall(L, LuaSandboxFunction_call_protected, &p);

	// Handle any error from Lua
	if (status != 0) {
		luasandbox_handle_error(p.sandbox, status TSRMLS_CC);
		RETVAL_FALSE;
	}

#if PHP_VERSION_ID < 70000
	// Delete varargs
	if (p.numArgs) {
		efree(p.args);
	}
#endif
}
/** }}} */

/** {{{ luasandbox_call_lua
 *
 * Much like lua_call, except it starts the appropriate timers and handles
 * errors as a PHP exception. Returns 1 on success, 0 on failure
 */
int luasandbox_call_lua(php_luasandbox_obj * sandbox, zval * sandbox_zval,
	int nargs, int nresults, int errfunc TSRMLS_DC)
{
	int status;
	int timer_started = 0;
#if PHP_VERSION_ID < 70000
	zval * old_zval;
#else
	zval old_zval;
#endif
	int was_paused;
	int old_allow_pause;

	// Initialise the CPU limit timer
	if (!sandbox->in_lua) {
		if (luasandbox_timer_is_expired(&sandbox->timer)) {
			zend_throw_exception(luasandboxtimeouterror_ce, luasandbox_timeout_message,
				LUA_ERRRUN TSRMLS_CC);
			return 0;
		}
		if (luasandbox_timer_start(&sandbox->timer)) {
			timer_started = 1;
		} else {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"Unable to start limit timer");
		}
	}

	// Save the current zval for later use in luasandbox_call_php. Restore it
	// after execution finishes, to support re-entrancy.
#if PHP_VERSION_ID < 70000
	old_zval = sandbox->current_zval;
	sandbox->current_zval = sandbox_zval;
#else
	ZVAL_COPY_VALUE(&old_zval, &sandbox->current_zval);
	ZVAL_COPY_VALUE(&sandbox->current_zval, sandbox_zval);
#endif

	// Make sure this is counted against the Lua usage time limit, and set the
	// allow_pause flag.
	was_paused = luasandbox_timer_is_paused(&sandbox->timer);
	luasandbox_timer_unpause(&sandbox->timer);
	old_allow_pause = sandbox->allow_pause;
	sandbox->allow_pause = ( !sandbox->in_lua || was_paused );

	// Call the function
	sandbox->in_lua++;
	status = lua_pcall(sandbox->state, nargs, nresults, errfunc);
	sandbox->in_lua--;
#if PHP_VERSION_ID < 70000
	sandbox->current_zval = old_zval;
#else
	ZVAL_COPY_VALUE(&sandbox->current_zval, &old_zval);
#endif

	// Restore pause state
	sandbox->allow_pause = old_allow_pause;
	if (was_paused) {
		luasandbox_timer_pause(&sandbox->timer);
	}

	// Stop the timer
	if (timer_started) {
		luasandbox_timer_stop(&sandbox->timer);
	}

	// Handle normal errors
	if (status) {
		luasandbox_handle_error(sandbox, status TSRMLS_CC);
		return 0;
	}

	return 1;
}
/* }}} */

/** {{{ luasandbox_call_helper
 *
 * Call the function at the top of the stack and then pop it. Set return_value
 * to an array containing all the results.
 */
static void luasandbox_call_helper(lua_State * L, zval * sandbox_zval, php_luasandbox_obj * sandbox,
	star_param_t args, int numArgs, zval * return_value TSRMLS_DC)
{
	// Save the top position
	int origTop = lua_gettop(L);
	// Keep track of the stack index where the return values will appear
	int retIndex = origTop + 2;
	int i, numResults;
	zval *v;

	// Check to see if the value is a valid function
	if (lua_type(L, -1) != LUA_TFUNCTION) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"the specified Lua value is not a valid function");
		lua_settop(L, origTop - 1);
		RETURN_FALSE;
	}

	// Push the error function
	lua_pushcfunction(L, luasandbox_attach_trace);

	// Push the function to be called
	lua_pushvalue(L, origTop);

	// Push the arguments
	if (!lua_checkstack(L, numArgs + 10)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"unable to allocate stack space for Lua call");
		lua_settop(L, origTop - 1);
		RETURN_FALSE;
	}
	for (i = 0; i < numArgs; i++) {
#if PHP_VERSION_ID < 70000
		v = *(args[i]);
#else
		v = &(args[i]);
#endif
		if (!luasandbox_push_zval(L, v, NULL)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"unable to convert argument %d to a lua value", i + 1);
			lua_settop(L, origTop - 1);
			RETURN_FALSE;
		}
	}

	// Call the function
	if (!luasandbox_call_lua(sandbox, sandbox_zval, numArgs, LUA_MULTRET, origTop + 1 TSRMLS_CC)) {
		lua_settop(L, origTop - 1);
		RETURN_FALSE;
	}

	// Calculate the number of results and create an array of that capacity
	numResults = lua_gettop(L) - retIndex + 1;
	array_init_size(return_value, numResults);

	// Fill the array with the results
	for (i = 0; i < numResults; i++) {
#if PHP_VERSION_ID < 70000
		zval * element;
		ALLOC_INIT_ZVAL(element); // ensure elem is inited in case we bail
		if (!luasandbox_lua_to_zval(element, L, retIndex + i, sandbox_zval, NULL TSRMLS_CC)) {
			// Convert failed (which means an exception), so bail.
			zval_ptr_dtor(&element);
			break;
		}
		zend_hash_next_index_insert(Z_ARRVAL_P(return_value),
			(void*)&element,
			sizeof(zval*), NULL);
#else
		zval element;
		ZVAL_NULL(&element); // ensure elem is inited in case we bail
		if (!luasandbox_lua_to_zval(&element, L, retIndex + i, sandbox_zval, NULL TSRMLS_CC)) {
			// Convert failed (which means an exception), so bail.
			zval_ptr_dtor(&element);
			break;
		}
		zend_hash_next_index_insert(Z_ARRVAL_P(return_value), &element);
#endif
	}

	// Balance the stack
	lua_settop(L, origTop - 1);
}
/* }}} */

/** {{{ luasandbox_find_field
 *
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
			lua_rawget(L, top + 1);

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

/** {{{ luasandbox_get_php_obj
 *
 * Get the object data for a lua state.
 */
php_luasandbox_obj * luasandbox_get_php_obj(lua_State * L)
{
	php_luasandbox_obj * obj;
	lua_getfield(L, LUA_REGISTRYINDEX, "php_luasandbox_obj");
	obj = (php_luasandbox_obj*)lua_touserdata(L, -1);
	assert(obj != NULL);
	lua_pop(L, 1);
	return obj;
}
/* }}} */

/** {{{ proto void LuaSandbox::registerLibrary(string libname, array functions)
 *
 * Register a set of PHP functions as a Lua library, so that Lua can call the
 * relevant PHP code.
 *
 * The first parameter is the name of the library. In the Lua state, the global
 * variable of this name will be set to the table of functions. If the table
 * already exists, the new functions will be added to it.
 *
 * The second parameter is an array, where each key is a function name, and
 * each value is a corresponding PHP callback.
 *
 * Both Lua and PHP allow functions to be called with any number of arguments.
 * The parameters to the Lua function will be passed through to the PHP.
 *
 * Lua supports multiple return values. The PHP function should return either
 * null (for zero return values) or an array of return values. The keys of the
 * return array will not be used, rather the values will be taken in their
 * internal order.
 *
 * For catching Lua errors that might be raised, we need a struct and a helper
 * function too.
 */

struct LuaSandbox_registerLibrary_params {
	char *libname;
	str_param_len_t libname_len;
	HashTable *functions;
};

static int LuaSandbox_registerLibrary_protected(lua_State* L) {
	struct LuaSandbox_registerLibrary_params *p = (struct LuaSandbox_registerLibrary_params *)lua_touserdata(L, 1);
	HashTable * functions = p->functions;

	// Determine if the library exists already
	// Make a copy of the library name on the stack for rawset later
	lua_pushlstring(L, p->libname, p->libname_len);
	lua_pushvalue(L, -1);
	lua_rawget(L, LUA_GLOBALSINDEX);
	if (lua_type(L, -1) == LUA_TNIL) {
		// Remove the nil
		lua_pop(L, 1);

		// Create the new table
		lua_createtable(L, 0, zend_hash_num_elements(functions));
	}

#if PHP_VERSION_ID < 70000
	HashPosition pos;
	for (zend_hash_internal_pointer_reset_ex(functions, &pos);
			zend_hash_get_current_key_type_ex(functions, &pos) != HASH_KEY_NON_EXISTANT;
			zend_hash_move_forward_ex(functions, &pos))
	{
		char * key = "";
		uint key_length = 0;
		ulong lkey = 0;
		zval ** callback;

		int key_type = zend_hash_get_current_key_ex(functions, &key, &key_length,
				&lkey, 0, &pos);
		zend_hash_get_current_data_ex(functions, (void**)&callback, &pos);

		// Push the key
		if (key_type == HASH_KEY_IS_STRING) {
			lua_pushlstring(L, key, key_length - 1);
		} else {
			lua_pushinteger(L, lkey);
		}

		// Push the callback zval and create the closure
		luasandbox_push_zval_userdata(L, *callback);
		lua_pushcclosure(L, luasandbox_call_php, 1);

		// Add it to the table
		lua_rawset(L, -3);
	}
#else
	ulong lkey;
	zend_string *key;
	zval *callback;
	ZEND_HASH_FOREACH_KEY_VAL(functions, lkey, key, callback)
	{
		// Push the key
		if ( key ) {
			lua_pushlstring(L, ZSTR_VAL(key), ZSTR_LEN(key));
		} else {
			lua_pushinteger(L, lkey);
		}

		// Push the callback zval and create the closure
		luasandbox_push_zval_userdata(L, callback);
		lua_pushcclosure(L, luasandbox_call_php, 1);

		// Add it to the table
		lua_rawset(L, -3);
	} ZEND_HASH_FOREACH_END();
#endif

	// Move the new table to the global namespace
	// The key is on the stack already
	lua_rawset(L, LUA_GLOBALSINDEX);

	return 0;
}

/** {{{ LuaSandbox::registerLibrary
 */
PHP_METHOD(LuaSandbox, registerLibrary) {
	struct LuaSandbox_registerLibrary_params p;
	lua_State * L;
	int status;
	zval * zfunctions = NULL;

	L = luasandbox_state_from_zval(getThis() TSRMLS_CC);

	CHECK_VALID_STATE(L);

	p.libname = NULL;
	p.libname_len = 0;
	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sa",
		&p.libname, &p.libname_len, &zfunctions) == FAILURE)
	{
		RETURN_FALSE;
	}

	p.functions = Z_ARRVAL_P(zfunctions);

	status = lua_cpcall(L, LuaSandbox_registerLibrary_protected, &p);

	// Handle any error from Lua
	if (status != 0) {
		luasandbox_handle_error(GET_LUASANDBOX_OBJ(getThis()), status TSRMLS_CC);
		RETVAL_FALSE;
	}
}

/** {{{ LuaSandbox::allowedGlobals
 */
PHP_METHOD(LuaSandbox, allowedGlobals) {
	HashTable * globals = &LUASANDBOX_G(allowed_globals);
	#if PHP_VERSION_ID < 70000
	MAKE_STD_ZVAL(return_value);
#endif
	if ( globals && globals->nNumOfElements ) {
		array_init_size( return_value, globals->nNumOfElements );
		zend_string * key;
		ZEND_HASH_FOREACH_STR_KEY(globals, key)
			add_next_index_stringl( return_value, ZSTR_VAL(key), ZSTR_LEN(key) );
		ZEND_HASH_FOREACH_END();
	} else {
		array_init_size(return_value, 1);
		char *msg;
		php_sprintf( msg, "No globals are allowed. Set %s in php.ini\n", PHP_LUASANDBOX_INI_ALLOWED_GLOBALS );
		add_next_index_string( return_value, msg );
	}
}
/* }}} */

/** {{{ LuaSandbox::additionalLibraries
 */
PHP_METHOD(LuaSandbox, additionalLibraries) {
#if PHP_VERSION_ID < 70000
	MAKE_STD_ZVAL(return_value);
#endif
	array_init_size( return_value, LUASANDBOX_G(library_loaders).nNumOfElements );
	zend_string * lib_z;
	ZEND_HASH_FOREACH_STR_KEY(&LUASANDBOX_G(additional_libraries), lib_z)
		add_next_index_stringl( return_value, ZSTR_VAL(lib_z), ZSTR_LEN(lib_z) );
	ZEND_HASH_FOREACH_END();
}
/* }}} */

/** {{{ luasandbox_instanceof
 * Based on is_derived_class in zend_object_handlers.c
 */
static zend_bool luasandbox_instanceof(
	zend_class_entry *child_class, zend_class_entry *parent_class)
{
	while (child_class) {
		if (child_class == parent_class) {
			return 1;
		}
		child_class = child_class->parent;
	}

	return 0;
}
/* }}} */

/** {{{ luasandbox_call_php
 *
 * The Lua callback for calling PHP functions. See the doc comment on
 * LuaSandbox::registerLibrary() for information about calling conventions.
 */
int luasandbox_call_php(lua_State * L)
{
	php_luasandbox_obj * intern = luasandbox_get_php_obj(L);

	luasandbox_enter_php(L, intern);

	zval * callback_p;
#if PHP_VERSION_ID < 70000
	callback_p = *(zval**)lua_touserdata(L, lua_upvalueindex(1));
#else
	callback_p = (zval*)lua_touserdata(L, lua_upvalueindex(1));
#endif
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	char *is_callable_error = NULL;
	int top = lua_gettop(L);
	int i;
	int num_results = 0;
	int status;
	HashTable * ht;
	TSRMLS_FETCH();

	// Based on zend_parse_arg_impl()
	if (zend_fcall_info_init(callback_p, 0, &fci, &fcc, NULL,
		&is_callable_error TSRMLS_CC) != SUCCESS)
	{
		// Handle errors similar to the way PHP does it: show a warning and
		// return nil
		php_error_docref(NULL TSRMLS_CC, E_WARNING,
			"to be a valid callback, %s", is_callable_error);
		efree(is_callable_error);
		lua_pushnil(L);
		luasandbox_leave_php(L, intern);
		return 1;
	}

#if PHP_VERSION_ID < 70000
	zval *retval_ptr = NULL;
	fci.retval_ptr_ptr = &retval_ptr;
#else
	zval retval;
	fci.retval = &retval;
#endif

	int args_failed = 0;
	star_param_t args;
#if PHP_VERSION_ID < 70000
	// Make an array of zval double-pointers to hold the arguments
	args = (zval***)ecalloc(top, sizeof(void*) * 2);
	zval **pointers = (zval**)(args + top);
	for (i = 0; i < top; i++ ) {
		args[i] = &(pointers[i]);
		ALLOC_INIT_ZVAL(pointers[i]); // ensure is inited in case we fail
		if (!luasandbox_lua_to_zval(pointers[i], L, i + 1, intern->current_zval, NULL TSRMLS_CC)) {
			// Argument conversion failed, so skip the call. The PHP exception
			// from the conversion will be handled below, along with freeing
			// all the zvals in pointers[0 <= i < top].
			args_failed = 1;
			top = i + 1;
			break;
		}
	}
#else
	// Make an array of zvals to hold the arguments
	args = (zval *)ecalloc(top, sizeof(zval));
	for (i = 0; i < top; i++ ) {
		ZVAL_NULL(&args[i]); // ensure is inited in case we fail
		if (!luasandbox_lua_to_zval(&args[i], L, i + 1, &intern->current_zval, NULL TSRMLS_CC)) {
			// Argument conversion failed, so skip the call. The PHP exception
			// from the conversion will be handled below, along with freeing
			// all the zvals in args[0 <= i < top].
			args_failed = 1;
			top = i + 1;
			break;
		}
	}
#endif

	if (!args_failed) {
		// Initialise the fci. Use zend_fcall_info_args_restore() since that's an
		// almost-legitimate way to avoid the extra malloc that we'd get from
		// zend_fcall_info_argp()
		zend_fcall_info_args_restore(&fci, top, args);

		// Sanity check, timers should never be paused at this point
		assert(!luasandbox_timer_is_paused(&intern->timer));

		// Call the function
		status = zend_call_function(&fci, &fcc TSRMLS_CC);

		// Automatically unpause now that PHP has returned
		luasandbox_timer_unpause(&intern->timer);

#if PHP_VERSION_ID < 70000
		if (status == SUCCESS && fci.retval_ptr_ptr && *fci.retval_ptr_ptr) {
			// Push the return values back to Lua
			if (Z_TYPE_PP(fci.retval_ptr_ptr) == IS_NULL) {
				// No action
			} else if (Z_TYPE_PP(fci.retval_ptr_ptr) == IS_ARRAY) {
				ht = Z_ARRVAL_PP(fci.retval_ptr_ptr);
				luaL_checkstack(L, zend_hash_num_elements(ht) + 10, "converting PHP return array to Lua");
				HashPosition p;
				for (zend_hash_internal_pointer_reset_ex(ht, &p);
						zend_hash_get_current_key_type_ex(ht, &p) != HASH_KEY_NON_EXISTANT;
						zend_hash_move_forward_ex(ht, &p))
				{
					zval ** value;
					zend_hash_get_current_data_ex(ht, (void**)&value, &p);
					luasandbox_push_zval(L, *value, NULL);
					num_results++;
				}
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"function tried to return a single value to Lua without wrapping it in an array");
			}
			zval_ptr_dtor(&retval_ptr);
		}
#else
		if (status == SUCCESS) {
			// Push the return values back to Lua
			if (Z_ISNULL_P(fci.retval) || Z_ISUNDEF_P(fci.retval)) {
				// No action
			} else if (Z_TYPE_P(fci.retval) == IS_ARRAY) {
				ht = Z_ARRVAL_P(fci.retval);
				luaL_checkstack(L, zend_hash_num_elements(ht) + 10, "converting PHP return array to Lua");

				zval *value;
				ZEND_HASH_FOREACH_VAL(ht, value)
				{
					luasandbox_push_zval(L, value, NULL);
					num_results++;
				} ZEND_HASH_FOREACH_END();
			} else {
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"function tried to return a single value to Lua without wrapping it in an array");
			}
			zval_ptr_dtor(&retval);
		}
#endif
	}

	// Free the argument zvals
	for (i = 0; i < top; i++) {
#if PHP_VERSION_ID < 70000
		zval_ptr_dtor(args[i]);
#else
		zval_ptr_dtor(&(args[i]));
#endif
	}
	efree(args);
	luasandbox_leave_php(L, intern);

	// If an exception occurred, convert it to a Lua error
	if (EG(exception)) {
		// Get the error message and push it to the stack
#if PHP_VERSION_ID < 70000
		zend_class_entry * ce = Z_OBJCE_P(EG(exception));
		zval * zmsg = zend_read_property(ce, EG(exception), "message", sizeof("message")-1, 1 TSRMLS_CC);
#else
		zval exception, rv;
		ZVAL_OBJ(&exception, EG(exception));
		zend_class_entry * ce = Z_OBJCE(exception);
		zval * zmsg = zend_read_property(ce, &exception, "message", sizeof("message")-1, 1, &rv);
#endif
		if (zmsg && Z_TYPE_P(zmsg) == IS_STRING) {
			lua_pushlstring(L, Z_STRVAL_P(zmsg), Z_STRLEN_P(zmsg));
		} else {
			lua_pushliteral(L, "[unknown exception]");
		}

		// If the exception was a LuaSandboxRuntimeError or a subclass, clear the
		// exception and raise a non-fatal (catchable) error
		if (luasandbox_instanceof(ce, luasandboxruntimeerror_ce)) {
			zend_clear_exception(TSRMLS_C);
		} else {
			luasandbox_wrap_fatal(L);
		}
		lua_error(L);
	}
	return num_results;
}
/* }}} */

/** {{{ string LuaSandboxFunction::dump()
 *
 * Dump the function as a precompiled binary blob. Returns a string which may
 * later be loaded by LuaSandbox::loadBinary(), in the same or a different
 * sandbox object.
 *
 * For catching Lua errors that might be raised, we need a struct and a helper
 * function too.
 */

struct LuaSandboxFunction_dump_params {
	php_luasandboxfunction_obj * func;
	zval *return_value;
};

static int LuaSandboxFunction_dump_protected(lua_State* L) {
	struct LuaSandboxFunction_dump_params *p = (struct LuaSandboxFunction_dump_params *)lua_touserdata(L, 1);
	zval *return_value = p->return_value;
	smart_str buf = {0};

	luasandbox_function_push(p->func, L);
	lua_dump(L, luasandbox_dump_writer, (void*)&buf);
	smart_str_0(&buf);
#if PHP_VERSION_ID < 70000
	if (buf.len) {
		RETVAL_STRINGL(buf.c, buf.len, 0);
#else
	if (buf.s) {
		RETVAL_STR(buf.s);
#endif
	} else {
		smart_str_free(&buf);
		RETVAL_EMPTY_STRING();
	}

	return 0;
}

PHP_METHOD(LuaSandboxFunction, dump)
{
	struct LuaSandboxFunction_dump_params p;
	lua_State * L;
	php_luasandbox_obj * sandbox;
	int status;

	p.return_value = return_value;
	if (!luasandbox_function_init(getThis(), &p.func, &L, &sandbox TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	// Call the function
	status = lua_cpcall(L, LuaSandboxFunction_dump_protected, &p);

	// Handle any error from Lua
	if (status != 0) {
		luasandbox_handle_error(sandbox, status TSRMLS_CC);
		RETVAL_FALSE;
	}
}
/* }}} */

/** {{{ luasandbox_dump_writer
 *
 * Writer function for LuaSandboxFunction::dump().
 */
static int luasandbox_dump_writer(lua_State * L, const void * p, size_t sz, void * ud)
{
	smart_str * buf = (smart_str *)ud;
	smart_str_appendl(buf, p, sz);
	return 0;
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
