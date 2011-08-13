dnl $Id$
dnl config.m4 for extension luasandbox

PHP_ARG_WITH(luasandbox, for luasandbox support,
[  --with-luasandbox             Include luasandbox support])

dnl TODO: make this find either lua or luajit as it is installed in various 
dnl distros. Make a define available to C to indicate luajit is available 
dnl as opposed to lua. Maybe use pkgconfig since the library file is in an
dnl odd place. Allow the user to specify the include directory manually.
dnl -- TS

if test "$PHP_LUASANDBOX" != "no"; then
	SEARCH_PREFIX="/usr/local /usr"	
	SEARCH_INCLUDE_SUBDIR="lua5.1 lua"
	LUA_LIB_NAME=lua5.1
	LUA_LIB_FILE=liblua5.1.so

	SEARCH_INCLUDE=
	for prefix in $SEARCH_PREFIX; do
		for subdir in $SEARCH_INCLUDE_SUBDIR; do
			SEARCH_INCLUDE="$SEARCH_INCLUDE $prefix/include/$subdir"
		done
	done

	LUA_INCLUDE_DIR=
	AC_MSG_CHECKING([for lua include files in default path])
	for i in $SEARCH_INCLUDE; do
		if test -r "$i/lua.h"; then
			LUA_INCLUDE_DIR=$i
			AC_MSG_RESULT(found in $i)
		fi
	done

	if test -z "$LUA_INCLUDE_DIR"; then
		AC_MSG_RESULT([not found])
		AC_MSG_ERROR([Please reinstall the lua distribution])
	fi
	
	PHP_ADD_INCLUDE($LUA_INCLUDE_DIR)

	LUA_LIB_PREFIX=
	AC_MSG_CHECKING([for lua library files])
	for i in $SEARCH_PREFIX; do
		if test -r "$i/lib/$LUA_LIB_FILE"; then
			AC_MSG_RESULT([found in $i])
			LUA_LIB_PREFIX=$i
		fi
	done

	if test -z "$LUA_LIB_PREFIX"; then
		AC_MSG_RESULT([not found])
		AC_MSG_ERROR([Please reinstall the lua distribution])
	fi

	PHP_ADD_LIBRARY_WITH_PATH($LUA_LIB_NAME, $LUA_LIB_PREFIX/lib, LUASANDBOX_SHARED_LIBADD)
	PHP_SUBST(LUASANDBOX_SHARED_LIBADD)
	PHP_NEW_EXTENSION(luasandbox, luasandbox.c, $ext_shared)
fi
