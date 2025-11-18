--TEST--
Test for unpack() integer overflow
--FILE--
<?php
$sandbox = new LuaSandbox;
try {
	$sandbox->loadString('unpack({}, 0, 2147483647)')->call();
} catch ( Exception $ex ) {
	echo "Exception: " . $ex->getMessage() . "\n";
}
--EXPECT--
Exception: [string ""]:1: too many results to unpack
