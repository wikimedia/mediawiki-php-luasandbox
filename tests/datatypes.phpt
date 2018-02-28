--TEST--
Data type round-tripping
--FILE--
<?php

function doTest( $test, $data ) {
	printf( "%-25s ", "$test:" );

	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	try {
		$ret = $sandbox->loadString( 'return ...' )->call( $data );
		if ( is_array( $ret[0] ) ) {
			ksort( $ret[0], SORT_STRING );
		}
		printf( "%s\n", preg_replace( '/\s+/', ' ', var_export( $ret[0], 1 ) ) );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}
}

doTest( 'null', null );
doTest( 'int', 123 );
if ( is_int( 17179869184 ) ) {
	doTest( 'long', 17179869184 );
} else {
	// Fake it for 32-bit systems
	printf( "%-25s %s\n", "long:", "17179869184" );
}
doTest( 'double', 3.125 );
doTest( 'NAN', NAN );
doTest( 'INF', INF );
doTest( 'true', true );
doTest( 'false', false );
doTest( 'string', 'foobar' );
doTest( 'empty string', '' );
doTest( 'string containing NULs', "foo\0bar" );
doTest( 'array', array( 'foo', 'bar' ) );
doTest( 'associative array', array( 'foo', 'bar' => 'baz' ) );

$var = 42;
doTest( 'array with reference', [ &$var ] );

$sandbox = new LuaSandbox;
$sandbox->setMemoryLimit( 100000 );
$sandbox->setCPULimit( 0.1 );
$func = $sandbox->wrapPhpFunction( function ( $x ) { return [ "FUNC: $x" ]; } );
try {
	$ret = $sandbox->loadString( 'return ...' )->call( $func );
	$ret2 = $ret[0]->call( "ok" );
	printf( "%-25s %s\n", "function, pass-through:", $ret2[0] );

	$ret = $sandbox->loadString( 'f = ...; return f( "ok" )' )->call( $func );
	printf( "%-25s %s\n", "function, called:", $ret[0] );

	$ret = $sandbox->loadString( 'return function ( x ) return "FUNC: " .. x end' )->call();
	$ret2 = $ret[0]->call( "ok" );
	printf( "%-25s %s\n", "function, returned:", $ret2[0] );
} catch ( LuaSandboxError $e ) {
	printf( "EXCEPTION: %s\n", $e->getMessage() );
}

// HHVM leaks it otherwise, and the warning makes the test fail
unset( $ret, $func, $sandbox );

--EXPECT--
null:                     NULL
int:                      123
long:                     17179869184
double:                   3.125
NAN:                      NAN
INF:                      INF
true:                     true
false:                    false
string:                   'foobar'
empty string:             ''
string containing NULs:   'foo' . "\0" . 'bar'
array:                    array ( 0 => 'foo', 1 => 'bar', )
associative array:        array ( 0 => 'foo', 'bar' => 'baz', )
array with reference:     array ( 0 => 42, )
function, pass-through:   FUNC: ok
function, called:         FUNC: ok
function, returned:       FUNC: ok
