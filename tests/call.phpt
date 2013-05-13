--TEST--
LuaSandboxFunction::call
--FILE--
<?php
$sandbox = new LuaSandbox;
var_dump( $sandbox->loadString( 'return 1' )->call() );

echo "Proper handling of circular tables returned by Lua: ";
$sandbox = new LuaSandbox;
try {
	$ret = $sandbox->loadString( 'local t = {}; t.t = t; return t' )->call();
	echo var_export( $ret, 1 ) . "\n";
} catch ( Exception $ex ) {
	echo "Exception: " . $ex->getMessage() . "\n";
}

echo "Proper handling of circular tables in Lua→PHP call: ";
$sandbox = new LuaSandbox;
$f = $sandbox->wrapPhpFunction( function () {
	echo func_num_args() . " args ok\n";
} );
try {
	$sandbox->loadString( 'local f = ...; local t = {}; t.t = t; f( t )' )->call( $f );
} catch ( Exception $ex ) {
	echo "Exception: " . $ex->getMessage() . "\n";
}

--EXPECT--
array(1) {
  [0]=>
  int(1)
}
Proper handling of circular tables returned by Lua: Exception: Cannot pass circular reference to PHP
Proper handling of circular tables in Lua→PHP call: Exception: Cannot pass circular reference to PHP
