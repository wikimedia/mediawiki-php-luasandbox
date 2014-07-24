# HHVM needs to link luasandbox to the c++ version of liblua
if (LUA_USE_CPP)
  INCLUDE("FindLua51cpp.cmake")
else(!LUA_USE_CPP)
  INCLUDE(FindLua51)
endif(LUA_USE_CPP)

if (!LUA51_FOUND)
  message(FATAL_ERROR "unable to find Lua 5.1")
endif()

# Parse version string from debian/changelog and use it to generate luasandbox_version.h
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" CHANGELOG)
string(REGEX REPLACE "\\s*php-luasandbox \\(([0-9._-]+).*" "\\1" LUASANDBOX_VERSION "${CHANGELOG}" )
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/luasandbox_version.h "\#define LUASANDBOX_VERSION \"${LUASANDBOX_VERSION}\"\n")

HHVM_COMPAT_EXTENSION(luasandbox
  alloc.c
  data_conversion.c
  library.c
  luasandbox.c
  timer.c
  luasandbox_lstrlib.c)

HHVM_ADD_INCLUDES(luasandbox ${LUA_INCLUDE_DIR})
HHVM_LINK_LIBRARIES(luasandbox ${LUA_LIBRARIES})
HHVM_SYSTEMLIB(luasandbox ext_luasandbox.php)
