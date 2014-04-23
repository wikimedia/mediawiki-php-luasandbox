--TEST--
LuaSandboxFunction::construct() is private
--SKIPIF--
<?php if (!extension_loaded("luasandbox")) print "skip"; ?>
--FILE--
<?php
new LuaSandboxFunction;
?>
--EXPECTF--
Fatal error: Call to private%S LuaSandboxFunction::__construct%S from %s in %s on line %d
