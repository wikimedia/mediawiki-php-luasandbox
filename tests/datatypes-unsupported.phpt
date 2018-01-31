--TEST--
Handling of unsupported datatypes
--FILE--
<?php

function doTest( $test, $data ) {
	printf( "%s ", "$test (call PHP->Lua):" );
	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	try {
		$ret = $sandbox->loadString( 'return 1' )->call( $data );
		printf( "%s\n", preg_replace( '/\s+/', ' ', var_export( $ret, 1 ) ) );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}

	printf( "%s ", "$test (return PHP->Lua):" );
	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	$f = $sandbox->wrapPhpFunction( function () use ( $data ) {
		return [ $data ];
	} );
	try {
		$sandbox->loadString( 'local f = ...; f()' )->call( $f );
		printf( "%s\n", preg_replace( '/\s+/', ' ', var_export( $ret, 1 ) ) );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}
}

function doTest2( $test, $lua ) {
	printf( "%s ", "$test (call Lua->PHP):" );
	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	$f = $sandbox->wrapPhpFunction( function ( $val ) {
		echo "PHP received " . preg_replace( '/\s+/', ' ', var_export( $val, 1 ) ) . "\n";
	} );
	try {
		$sandbox->loadString( "local f = ...\n$lua\nf(v)" )->call( $f );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}

	printf( "%s ", "$test (return Lua->PHP):" );
	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	try {
		$ret = $sandbox->loadString( "$lua\nreturn v" )->call();
		printf( "%s\n", preg_replace( '/\s+/', ' ', var_export( $ret, 1 ) ) );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}
}

$test = array();
$test['foo'] = &$test;
doTest( 'recursive array', $test );

$test = new stdClass;
doTest( 'object', $test );

doTest2( 'recursive table', 'v = {}; v.v = v' );

--EXPECTF--
recursive array (call PHP->Lua): %AWarning: LuaSandboxFunction::call(): Cannot pass circular reference to Lua in %s on line %d
%AWarning: LuaSandboxFunction::call(): unable to convert argument 1 to a lua value in %s on line %d
false
recursive array (return PHP->Lua): %AWarning: LuaSandboxFunction::call(): Cannot pass circular reference to Lua in %s on line %d
false
object (call PHP->Lua): %AWarning: LuaSandboxFunction::call(): Unable to convert object of type stdClass in %s on line %d
%AWarning: LuaSandboxFunction::call(): unable to convert argument 1 to a lua value in %s on line %d
false
object (return PHP->Lua): %AWarning: LuaSandboxFunction::call(): Unable to convert object of type stdClass in %s on line %d
false
recursive table (call Lua->PHP): EXCEPTION: Cannot pass circular reference to PHP
recursive table (return Lua->PHP): EXCEPTION: Cannot pass circular reference to PHP
