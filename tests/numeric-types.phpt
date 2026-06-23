--TEST--
Numeric types returned from Lua
--FILE--
<?php

$sandbox = new LuaSandbox;
$sandbox->setMemoryLimit( 1000000 );
$sandbox->setCPULimit( 1 );

// Lua 5.3 and later have a distinct integer subtype, which is passed through
// to PHP. Lua 5.1 has only floats, so an integral value is converted to a PHP
// integer where that can be done without loss of precision.
preg_match( '/(\d+\.\d+)/', LuaSandbox::getVersionInfo()['Lua'], $m );
$hasIntegers = version_compare( $m[1], '5.3', '>=' );

// Lua expression => [ expected type on 5.1, expected type on 5.3+ ]
$cases = [
	'2' => [ 'integer', 'integer' ],
	'-2' => [ 'integer', 'integer' ],
	'2.0' => [ 'integer', 'double' ],
	'3 / 2' => [ 'double', 'double' ],
	'2 ^ 53' => [ 'double', 'double' ],
	'1e300' => [ 'double', 'double' ],
	'math.random( 1, 10 )' => [ 'integer', 'integer' ],
];

foreach ( $cases as $expr => list( $old, $new ) ) {
	$expected = $hasIntegers ? $new : $old;
	$actual = gettype( $sandbox->loadString( "return $expr" )->call()[0] );
	printf( "%-22s %s\n", $expr,
		$actual === $expected ? 'ok' : "FAIL: got $actual, expected $expected" );
}

// Line numbers in a Lua trace are integers regardless of the Lua version
try {
	$sandbox->loadString( "\n\nerror( 'boom' )" )->call();
	print "trace                  FAIL: no error thrown\n";
} catch ( LuaSandboxRuntimeError $e ) {
	$frame = $e->luaTrace[1];
	$actual = gettype( $frame['currentline'] ) . '/' . gettype( $frame['linedefined'] );
	printf( "%-22s %s\n", 'trace line numbers',
		$actual === 'integer/integer' ? 'ok' : "FAIL: got $actual" );
}

--EXPECT--
2                      ok
-2                     ok
2.0                    ok
3 / 2                  ok
2 ^ 53                 ok
1e300                  ok
math.random( 1, 10 )   ok
trace line numbers     ok
