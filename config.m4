dnl $Id$
dnl config.m4 for extension luasandbox

PHP_ARG_WITH(luasandbox, for luasandbox support,
[  --with-luasandbox             Include luasandbox support])

if test "$PHP_LUASANDBOX" != "no"; then
	dnl Include pkg-config macros definitions:
	m4_include([m4/pkg.m4])
	PKG_PROG_PKG_CONFIG

	dnl We need lua or fallback to luajit.
	dnl Under debian package is known as 'lua5.1'
	PKG_CHECK_MODULES([LUA], [lua],, [
		PKG_CHECK_MODULES([LUA], [lua5.1],, [
			PKG_CHECK_MODULES([LUA], [luajit])
		])
	])

	dnl LUA_LIBS and LUA_CFLAGS interprets them:
	PHP_EVAL_INCLINE($LUA_CFLAGS)
	PHP_EVAL_LIBLINE($LUA_LIBS, LUASANDBOX_SHARED_LIBADD)

	dnl Output results for debugging purposes:
	AC_MSG_NOTICE(LUA: found LUA_CFLAGS: $LUA_CFLAGS)
	AC_MSG_NOTICE(LUA: found LUA_LIBS: $LUA_LIBS)

	PHP_SUBST(LUASANDBOX_SHARED_LIBADD)
	PHP_NEW_EXTENSION(luasandbox, luasandbox.c timer.c, $ext_shared)
fi
