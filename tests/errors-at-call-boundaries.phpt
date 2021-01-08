--TEST--
Errors at PHP→Lua call boundaries
--FILE--
<?php

$sandbox = null; // Will be filled in later

function doTest( $str, callable $c ) {
	global $sandbox;

	echo "$str: ";
	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	try {
		$ret = $sandbox->loadString( 'local f = ...; return f()' )
			->call( $sandbox->wrapPhpFunction( $c ) );
		var_dump( $ret );
	} catch ( Exception $ex ) {
		echo "Exception: " . $ex->getMessage() . "\n";
	}
}

doTest( 'LuaSandbox::callFunction', function () {
	global $sandbox;

	$sandbox->loadString( 'function foo() return "no error" end' )->call();
	return $sandbox->callFunction( 'foo',
		str_repeat( 'a', 33334 ),
		str_repeat( 'b', 33334 ),
		str_repeat( 'c', 33334 ),
		str_repeat( 'd', 33334 )
	);
} );

doTest( 'LuaSandbox::registerLibrary 1', function () {
	global $sandbox;

	$sandbox->registerLibrary( str_repeat( 'a', 33334 ), [ 'foo' => function () {} ] );
	$sandbox->registerLibrary( str_repeat( 'b', 33334 ), [ 'foo' => function () {} ] );
	$sandbox->registerLibrary( str_repeat( 'c', 33334 ), [ 'foo' => function () {} ] );
	$sandbox->registerLibrary( str_repeat( 'd', 33334 ), [ 'foo' => function () {} ] );

	return [ 'no error' ];
} );

doTest( 'LuaSandbox::registerLibrary 2', function () {
	global $sandbox;

	$sandbox->registerLibrary( 'foo', [
		str_repeat( 'a', 33334 ) => function () {},
		str_repeat( 'b', 33334 ) => function () {},
		str_repeat( 'c', 33334 ) => function () {},
		str_repeat( 'd', 33334 ) => function () {},
	] );

	return [ 'no error' ];
} );

doTest( 'LuaSandboxFunction::call', function () {
	global $sandbox;

	return $sandbox->loadString( 'return "no error"' )->call(
		str_repeat( 'a', 33334 ),
		str_repeat( 'b', 33334 ),
		str_repeat( 'c', 33334 ),
		str_repeat( 'd', 33334 )
	);
} );

--EXPECT--
LuaSandbox::callFunction: Exception: not enough memory
LuaSandbox::registerLibrary 1: Exception: not enough memory
LuaSandbox::registerLibrary 2: Exception: not enough memory
LuaSandboxFunction::call: Exception: not enough memory
