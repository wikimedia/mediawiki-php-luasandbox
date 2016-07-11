--TEST--
LuaSandboxFunction::construct() is private
--SKIPIF--
<?php if (!extension_loaded("luasandbox")) print "skip"; ?>
--FILE--
<?php
new LuaSandboxFunction;
?>
--EXPECTF--
%AFatal error:%sCall to private%S LuaSandboxFunction::__construct%S from %a
