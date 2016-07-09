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
doTest( 'long', 17179869184 );
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
doTest( 'object', (object)array( 'foo' => 1, 'bar' => 'baz' ) );
doTest( 'object with numeric keys', (object)array( 'foo', 'bar' ) );

$var = 42;
doTest( 'array with reference', [ &$var ] );

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
object:                   array ( 'bar' => 'baz', 'foo' => 1, )
object with numeric keys: array ( 0 => 'foo', 1 => 'bar', )
array with reference:     array ( 0 => 42, )
