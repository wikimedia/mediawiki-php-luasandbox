
#ifndef PHP_LUASANDBOX_H
#define PHP_LUASANDBOX_H

#include <lua.h>

extern zend_module_entry luasandbox_module_entry;
extern char luasandbox_timeout_message[];

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
PHP_RSHUTDOWN_FUNCTION(luasandbox);
PHP_MINFO_FUNCTION(luasandbox);

PHP_METHOD(LuaSandbox, loadString);
PHP_METHOD(LuaSandbox, doString);
PHP_METHOD(LuaSandbox, setMemoryLimit);
PHP_METHOD(LuaSandbox, setCPULimit);
PHP_METHOD(LuaSandbox, callFunction);
PHP_METHOD(LuaSandbox, register);

PHP_METHOD(LuaSandboxFunction, call);

ZEND_BEGIN_MODULE_GLOBALS(luasandbox)
	int signal_handler_installed;
	struct sigaction old_handler;
ZEND_END_MODULE_GLOBALS(luasandbox)

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
	int in_php;
	volatile int timed_out;
	volatile int emergency_timed_out;
	int is_cpu_limited;
	struct timespec cpu_normal_limit;
	struct timespec cpu_emergency_limit;
};
typedef struct _php_luasandbox_obj php_luasandbox_obj;

struct _php_luasandboxfunction_obj {
	zend_object std;
	zval * sandbox;
	int index;
};
typedef struct _php_luasandboxfunction_obj php_luasandboxfunction_obj;

#endif	/* PHP_LUASANDBOX_H */

