--TEST--
Lua 5.1 math/table functions removed upstream, restored by LuaSandbox
--FILE--
<?php

$sandbox = new LuaSandbox;
$sandbox->setMemoryLimit( 1000000 );
$sandbox->setCPULimit( 1 );

function run( $sandbox, $name, $code ) {
	try {
		$ret = $sandbox->loadString( $code, $name )->call();
		return 'ok: ' . var_export( $ret[0] ?? null, true );
	} catch ( LuaSandboxError $e ) {
		return 'error: ' . $e->getMessage();
	}
}

// Lua 5.3 and later have a distinct integer subtype (see numeric-types.phpt);
// these functions all push their results the same way Lua 5.1 itself did,
// so a whole-number result is a PHP integer on 5.1 and a PHP float on 5.3+.
preg_match( '/(\d+\.\d+)/', LuaSandbox::getVersionInfo()['Lua'], $m );
$hasIntegers = version_compare( $m[1], '5.3', '>=' );
function num( $hasIntegers, $v ) {
	return $hasIntegers ? (float)$v : (int)$v;
}

$cases = [
	'math.atan2' => [ 'return math.atan2( 0, 5 )', fn ( $i ) => num( $i, 0 ) ],
	'math.cosh' => [ 'return math.cosh( 0 )', fn ( $i ) => num( $i, 1 ) ],
	'math.sinh' => [ 'return math.sinh( 0 )', fn ( $i ) => num( $i, 0 ) ],
	'math.tanh' => [ 'return math.tanh( 0 )', fn ( $i ) => num( $i, 0 ) ],
	'math.pow' => [ 'return math.pow( 2, 10 )', fn ( $i ) => num( $i, 1024 ) ],
	'math.ldexp' => [ 'return math.ldexp( 0.5, 4 )', fn ( $i ) => num( $i, 8 ) ],
	'math.log10' => [ 'return math.log10( 1000 )', fn ( $i ) => num( $i, 3 ) ],
	'math.frexp' => [
		'local m, e = math.frexp( 8 ); return m .. "," .. e', fn ( $i ) => '0.5,4' ],
	'table.getn' => [ 'return table.getn( { 1, 2, 3 } )', fn ( $i ) => 3 ],
	'table.getn (empty)' => [ 'return table.getn( {} )', fn ( $i ) => 0 ],
	'table.foreach' => [ '
		local sum = 0
		table.foreach( { 1, 2, 3 }, function ( k, v ) sum = sum + v end )
		return sum
	', fn ( $i ) => 6 ],
	'table.foreach (early exit)' => [ '
		local seen = {}
		local r = table.foreach( { 10, 20, 30 }, function ( k, v )
			table.insert( seen, v )
			if v == 20 then return "stopped" end
		end )
		return r .. ":" .. table.concat( seen, "," )
	', fn ( $i ) => 'stopped:10,20' ],
	'table.foreachi' => [ '
		local sum = 0
		table.foreachi( { 5, 6, 7 }, function ( i, v ) sum = sum + i * v end )
		return sum
	', fn ( $i ) => 38 ],
	'table.maxn (sparse)' => [
		'local t = {}; t[10] = "x"; t[3.5] = "y"; return table.maxn( t )',
		fn ( $i ) => num( $i, 10 ) ],
	'table.maxn (empty)' => [ 'return table.maxn( {} )', fn ( $i ) => num( $i, 0 ) ],
];

foreach ( $cases as $label => [ $code, $expectFn ] ) {
	$expected = $expectFn( $hasIntegers );
	$result = run( $sandbox, $label, $code );
	$expectedStr = 'ok: ' . var_export( $expected, true );
	printf( "%-28s %s\n", $label, $result === $expectedStr ? 'ok' : "FAIL: $result" );
}

// table.setn was already unsupported on a standard (non-LUA_COMPAT_GETN)
// Lua 5.1 build, raising this same error; LuaSandbox reproduces that on
// every version rather than silently discarding the requested size or
// implementing a working setn from scratch.
printf( "%-28s %s\n", 'table.setn',
	run( $sandbox, 'setn', 'local t = {1,2,3}; table.setn( t, 5 )' )
		=== "error: [string \"setn\"]:1: 'setn' is obsolete" ? 'ok' : 'FAIL' );

--EXPECT--
math.atan2                   ok
math.cosh                    ok
math.sinh                    ok
math.tanh                    ok
math.pow                     ok
math.ldexp                   ok
math.log10                   ok
math.frexp                   ok
table.getn                   ok
table.getn (empty)           ok
table.foreach                ok
table.foreach (early exit)   ok
table.foreachi               ok
table.maxn (sparse)          ok
table.maxn (empty)           ok
table.setn                   ok
