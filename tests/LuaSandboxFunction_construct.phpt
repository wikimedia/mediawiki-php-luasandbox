--TEST--
LuaSandboxFunction::construct() is private
--SKIPIF--
<?php if (!extension_loaded("luasandbox")) print "skip"; ?>
--FILE--
<?php
new LuaSandboxFunction;
?>
--EXPECTF--
Fatal error: Call to private LuaSandboxFunction::__construct() from invalid context in %s on line %d
