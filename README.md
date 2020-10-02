LuaSandbox is an extension for PHP 5, PHP 7, and HHVM to allow safely
running untrusted Lua 5.1 code from within PHP, which will generally
be faster than shelling out to a Lua binary and using inter-process
communication.

This is a fork created for traditio.wiki.

What's new:
 - Added luasandbox.allowed_globals to php.ini and LuaSandbox::allowedGlobals() method
 - Added luasandbox.additional_libraries to php.ini and LuaSandbox::additionalLibraries() method

For more details see <https://www.mediawiki.org/wiki/LuaSandbox>.
