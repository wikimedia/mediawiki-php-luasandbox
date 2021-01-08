--TEST--
Exception in a PHP function called from Lua
--FILE--
<?php

function throw_exception() {
	throw new Exception('message');
}
$sandbox = new LuaSandbox;
$sandbox->registerLibrary( 'test', array( 'throw_exception' => 'throw_exception' ) );
$f = $sandbox->loadString('test.throw_exception()');
try {
	$f->call();
} catch ( Exception $e ) {
	print $e->getMessage();
}

--EXPECT--
message
