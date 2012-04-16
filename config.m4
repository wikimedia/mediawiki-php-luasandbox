dnl $Id$
dnl config.m4 for extension luasandbox

AC_PREREQ(2.50)

PHP_ARG_WITH(luasandbox, for luasandbox support,
[  --with-luasandbox             Include luasandbox support])
dnl RT is not available on Mac OS X so we need an easy way to
dnl disable librt.
PHP_ARG_WITH(librt, [whether to include librt],
[  --without-librt    Do not attempt to use librt, forced on Mac OS X], yes, no)

if test "$PHP_LUASANDBOX" != "no"; then
	dnl Include pkg-config macros definitions:
	m4_include([m4/pkg.m4])

	dnl ICU did not support pkg-config till recently; current WM version
	dnl probably does not support it as well
	m4_include([m4/ac_check_icu.m4])
	PKG_PROG_PKG_CONFIG

	dnl We need lua or fallback to luajit.
	dnl Under debian package is known as 'lua5.1'
	PKG_CHECK_MODULES([LUA], [lua],, [
		PKG_CHECK_MODULES([LUA], [lua5.1],, [
			PKG_CHECK_MODULES([LUA], [luajit])
		])
	])

	AC_CHECK_ICU( [4.0] )

	AC_MSG_CHECKING([whether to use librt])
	dnl librt does not exist on Mac OS X
	case "$host_alias" in
	*darwin*)
		AC_MSG_RESULT([no, not available on Mac OS X])
		PHP_LIBRT="no"
	;;
	*)
		AC_MSG_RESULT($PHP_LIBRT)
	;;
	esac

	if test "$PHP_LIBRT" != "no"; then
		PKG_CHECK_MODULES([RT], [rt])
	fi

	dnl LUA_LIBS and LUA_CFLAGS interprets them:
	PHP_EVAL_INCLINE($LUA_CFLAGS)
	PHP_EVAL_LIBLINE($LUA_LIBS, LUASANDBOX_SHARED_LIBADD)

	PHP_EVAL_INCLINE($ICU_CFLAGS)
	PHP_EVAL_LIBLINE($ICU_LIBS, LUASANDBOX_SHARED_LIBADD)

	PHP_EVAL_INCLINE($RT_CFLAGS)
	PHP_EVAL_LIBLINE($RT_LIBS, LUASANDBOX_SHARED_LIBADD)

	PHP_SUBST(LUASANDBOX_SHARED_LIBADD)
	PHP_NEW_EXTENSION(luasandbox, alloc.c data_conversion.c library.c luasandbox.c timer.c ustring.c, $ext_shared)
fi
