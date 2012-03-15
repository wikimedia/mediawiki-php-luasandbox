--TEST--
LuaSandboxFunction::call
--FILE--
<?php
$sandbox = new LuaSandbox;
var_dump( $sandbox->loadString( 'return 1' )->call() );
--EXPECT--
array(1) {
  [0]=>
  int(1)
}
