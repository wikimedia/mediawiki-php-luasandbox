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

echo "Passing lots of arguments PHP->Lua doesn't cause a crash: ";
$sandbox = new LuaSandbox;
$ret = call_user_func_array(
	array( $sandbox->loadString( 'return select( "#", ... )' ), 'call' ),
	array_fill( 0, 500, '' )
);
echo "$ret[0] args ok\n";

echo "Passing lots of arguments Lua->PHP doesn't cause a crash: ";
$sandbox = new LuaSandbox;
$f = $sandbox->wrapPhpFunction( function () {
	echo func_num_args() . " args ok\n";
} );
$sandbox->loadString( 'local f = ...; f( string.byte( string.rep( "x", 500 ), 1, -1 ) )' )->call( $f );

echo "Returning lots of values PHP->Lua doesn't cause a crash: ";
$sandbox = new LuaSandbox;
$f = $sandbox->wrapPhpFunction( function () {
	return array_fill( 0, 500, '' );
} );
$ret = $sandbox->loadString( 'local f = ...; return select( "#", f() )' )->call( $f );
echo "$ret[0] values ok\n";

echo "Returning lots of values Lua->PHP doesn't cause a crash: ";
$sandbox = new LuaSandbox;
$ret = $sandbox->loadString( 'return string.byte( string.rep( "x", 500 ), 1, -1 )' )->call();
echo count( $ret ) . " values ok\n";

echo "Passing deeply-nested arrays PHP->Lua doesn't cause a crash: ";
$sandbox = new LuaSandbox;
$v = 1;
for ( $i = 0; $i < 500; $i++ ) {
	$v = array( $v );
}
$lua = <<<LUA
	local ct, t = 0, ...
	while type( t ) == "table" do
		_, t = next( t )
		ct = ct + 1
	end
	return ct
LUA;
$ret = $sandbox->loadString( $lua )->call( $v );
echo "$ret[0] levels ok\n";

echo "Passing deeply-nested tables Lua->PHP doesn't cause a crash: ";
$sandbox = new LuaSandbox;
$ret = $sandbox->loadString( 'local t = 1; for i = 1, 500 do t = { t } end; return t' )->call();
$ct = 0;
$v = $ret[0];
while ( is_array( $v ) ) {
	$v = reset( $v );
	$ct++;
}
echo "$ct levels ok\n";

--EXPECT--
array(1) {
  [0]=>
  int(1)
}
Proper handling of circular tables returned by Lua: Exception: Cannot pass circular reference to PHP
Proper handling of circular tables in Lua→PHP call: Exception: Cannot pass circular reference to PHP
Passing lots of arguments PHP->Lua doesn't cause a crash: 500 args ok
Passing lots of arguments Lua->PHP doesn't cause a crash: 500 args ok
Returning lots of values PHP->Lua doesn't cause a crash: 500 values ok
Returning lots of values Lua->PHP doesn't cause a crash: 500 values ok
Passing deeply-nested arrays PHP->Lua doesn't cause a crash: 500 levels ok
Passing deeply-nested tables Lua->PHP doesn't cause a crash: 500 levels ok
