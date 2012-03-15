--TEST--
dump -> loadBinary -> call
--FILE--
<?php

var_dump( $sandbox = new LuaSandbox );
var_dump( $f = $sandbox->loadString( 'return 1' ) );
$dump = $f->dump();
var_dump( $restore = $sandbox->loadBinary( $dump ) );
var_dump( $restore->call() );
--EXPECT--
object(LuaSandbox)#1 (0) {
}
object(LuaSandboxFunction)#2 (0) {
}
object(LuaSandboxFunction)#3 (0) {
}
array(1) {
  [0]=>
  int(1)
}
