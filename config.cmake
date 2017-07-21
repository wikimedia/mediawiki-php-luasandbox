# HHVM needs to link luasandbox to the c++ version of liblua
if (LUA_USE_CPP)
  INCLUDE("FindLua51cpp.cmake")
else(!LUA_USE_CPP)
  INCLUDE(FindLua51)
endif(LUA_USE_CPP)

if (!LUA51_FOUND)
  message(FATAL_ERROR "unable to find Lua 5.1")
endif()

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
