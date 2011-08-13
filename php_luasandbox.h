
#ifndef PHP_LUASANDBOX_H
#define PHP_LUASANDBOX_H

#include <lua.h>

extern zend_module_entry luasandbox_module_entry;
#define phpext_luasandbox_ptr &luasandbox_module_entry

#ifdef PHP_WIN32
#	define PHP_LUASANDBOX_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_LUASANDBOX_API __attribute__ ((visibility("default")))
#else
#	define PHP_LUASANDBOX_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(luasandbox);
PHP_MSHUTDOWN_FUNCTION(luasandbox);
PHP_MINFO_FUNCTION(luasandbox);

PHP_METHOD(LuaSandbox, loadString);
PHP_METHOD(LuaSandbox, setMemoryLimit);
PHP_METHOD(LuaSandbox, callFunction);
PHP_METHOD(LuaSandbox, callScript);
PHP_METHOD(LuaSandbox, createFunction);

/* 
  	Declare any global variables you may need between the BEGIN
	and END macros here:     

ZEND_BEGIN_MODULE_GLOBALS(luasandbox)
	long  global_value;
	char *global_string;
ZEND_END_MODULE_GLOBALS(luasandbox)
*/

/* In every utility function you add that needs to use variables 
   in php_luasandbox_globals, call TSRMLS_FETCH(); after declaring other 
   variables used by that function, or better yet, pass in TSRMLS_CC
   after the last function argument and declare your utility function
   with TSRMLS_DC after the last declared argument.  Always refer to
   the globals in your function as LUASANDBOX_G(variable).  You are 
   encouraged to rename these macros something shorter, see
   examples in any other php module directory.
*/

#ifdef ZTS
#define LUASANDBOX_G(v) TSRMG(luasandbox_globals_id, zend_luasandbox_globals *, v)
#else
#define LUASANDBOX_G(v) (luasandbox_globals.v)
#endif

struct _php_luasandbox_obj {
	zend_object std;
	lua_State * state;
	size_t memory_limit;
	size_t memory_usage;
};
typedef struct _php_luasandbox_obj php_luasandbox_obj;

#endif	/* PHP_LUASANDBOX_H */

