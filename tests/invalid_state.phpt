--TEST--
Functions called on an invalid LuaSandbox, after emergency timeout
--SKIPIF--
<?php if(defined("HPHP_VERSION")) print "skip"; ?>
--FILE--
<?php
$sandbox = new LuaSandbox;
$sandbox->setCPULimit(100, 0.1);
$f = $sandbox->loadString('while true do end');
$dump = $f->dump();
try {
	$f->call();
} catch (LuaSandboxEmergencyTimeoutError $e) {
	print $e->getMessage() . "\n";
}
$f->call();
$sandbox->loadString('foo()');
$sandbox->loadBinary($dump);
$sandbox->callFunction('foo');
--EXPECTF--
The maximum execution time was exceeded and the current Lua statement failed to return, leading to destruction of the Lua state

Warning: LuaSandboxFunction::call(): invalid LuaSandbox state in %s on line %d

Warning: LuaSandbox::loadString(): invalid LuaSandbox state in %s on line %d

Warning: LuaSandbox::loadBinary(): invalid LuaSandbox state in %s on line %d

Warning: LuaSandbox::callFunction(): invalid LuaSandbox state in %s on line %d
