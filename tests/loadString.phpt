--TEST--
loadString 1
--FILE--
<?php
var_dump($sandbox = new LuaSandbox);
var_dump($f = $sandbox->loadString('foo()'));
try {
	$f = $sandbox->loadString('foo');
} catch( Exception $e ) {
	print $e->getMessage();
}

--EXPECTF--
object(LuaSandbox)#1 (0) {
}
object(LuaSandboxFunction)#2 (0) {
}
[string ""]:1: '=' expected near '<eof>'
