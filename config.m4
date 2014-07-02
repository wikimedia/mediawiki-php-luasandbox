dnl $Id$
dnl config.m4 for extension luasandbox

AC_PREREQ(2.50)

PHP_ARG_WITH(luasandbox, for luasandbox support,
[  --with-luasandbox             Include luasandbox support])

$SED -n -e '0,/^php-luasandbox/{s/php-luasandbox (\(.*\)).*/#define LUASANDBOX_VERSION "\1"/p}' \
        $srcdir/debian/changelog > $srcdir/luasandbox_version.h

if test "$PHP_LUASANDBOX" != "no"; then
	dnl Include pkg-config macros definitions:
	m4_include([m4/pkg.m4])
	PKG_PROG_PKG_CONFIG

	dnl We need lua or fallback to luajit.
	dnl Under debian package is known as 'lua5.1'
	dnl Under freebsd package is known as 'lua-5.1'
	PKG_CHECK_MODULES([LUA], [lua >= 5.1 lua < 5.2],, [
		PKG_CHECK_MODULES([LUA], [lua5.1],, [
			PKG_CHECK_MODULES([LUA], [lua-5.1],, [
				PKG_CHECK_MODULES([LUA], [luajit])
			])
		])
	])

	dnl Timers require real-time and pthread library on Linux and not
	dnl supported on other platforms
	AC_SEARCH_LIBS([timer_create], [rt], [
		PHP_EVAL_LIBLINE($LIBS, LUASANDBOX_SHARED_LIBADD)
	])
	AC_SEARCH_LIBS([sem_init], [pthread], [
		PHP_EVAL_LIBLINE($LIBS, LUASANDBOX_SHARED_LIBADD)
	])

	dnl LUA_LIBS and LUA_CFLAGS interprets them:
	PHP_EVAL_INCLINE($LUA_CFLAGS)
	PHP_EVAL_LIBLINE($LUA_LIBS, LUASANDBOX_SHARED_LIBADD)

	PHP_SUBST(LUASANDBOX_SHARED_LIBADD)
	PHP_NEW_EXTENSION(luasandbox, alloc.c data_conversion.c library.c luasandbox.c timer.c luasandbox_lstrlib.c, $ext_shared)
	PHP_ADD_MAKEFILE_FRAGMENT
fi
