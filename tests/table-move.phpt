--TEST--
table.move: safe to expose, and matches the native semantics
--FILE--
<?php

function run( $name, $code, $cpuLimit = 1 ) {
	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 10000000 );
	$sandbox->setCPULimit( $cpuLimit );
	try {
		$ret = $sandbox->loadString( $code, $name )->call();
		return 'ok: ' . var_export( $ret[0] ?? null, true );
	} catch ( LuaSandboxError $e ) {
		return 'error: ' . $e->getMessage();
	}
}

$cases = [
	'basic copy' => [ '
		local t = { 1, 2, 3 }
		local d = {}
		table.move( t, 1, 3, 1, d )
		return table.concat( d, "," )
	', 'ok: \'1,2,3\'' ],

	// Overlapping ranges within the same table must behave like memmove:
	// values that get overwritten are read before the write happens.
	'overlap, destination after source' => [ '
		local t = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }
		table.move( t, 1, 5, 3 )
		return table.concat( t, "," )
	', 'ok: \'1,2,1,2,3,4,5,8,9,10\'' ],
	'overlap, destination before source' => [ '
		local t = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }
		table.move( t, 3, 8, 1 )
		return table.concat( t, "," )
	', 'ok: \'3,4,5,6,7,8,7,8,9,10\'' ],

	'default destination is the source table' => [ '
		local t = { 10, 20, 30 }
		local r = table.move( t, 1, 3, 5 )
		return tostring( r == t ) .. ":" .. t[5] .. "," .. t[6] .. "," .. t[7]
	', 'ok: \'true:10,20,30\'' ],
	'returns the destination table' => [ '
		local t, d = { 1, 2, 3 }, {}
		return table.move( t, 1, 3, 1, d ) == d
	', 'ok: true' ],
	'empty range leaves both tables untouched' => [ '
		local t = { 1, 2, 3 }
		local r = table.move( t, 5, 3, 1 )
		return tostring( r == t ) .. ":" .. table.concat( t, "," )
	', 'ok: \'true:1,2,3\'' ],

	// Only integer-valued keys within [f, e] participate; everything else
	// in the table -- including non-numeric keys -- is left alone.
	'non-participating keys are untouched' => [ '
		local t = { [2] = "b", [5] = "e", [100] = "z", label = "x" }
		local d = { existing = "keep me" }
		table.move( t, 1, 10, 1, d )
		return ( d[2] or "?" ) .. "," .. ( d[5] or "?" ) .. ","
			.. tostring( d[100] ) .. "," .. d.existing
	', 'ok: \'b,e,nil,keep me\'' ],

	'non-table source errors' => [
		'table.move( "x", 1, 2, 1 )',
		"error: [string \"non-table source errors\"]:1: bad argument #1 to 'move' (table expected, got string)" ],

	// The whole point: table.move(t, 1, 2^40, 1, {}) used to be rejected
	// outright because the native implementation copies the requested
	// range in a C loop the CPU limit hook can't interrupt. LuaSandbox's
	// table.move walks the source table's real entries instead, so its
	// cost is bounded by the table's actual size (2 entries here) and not
	// by the requested range -- it now succeeds, quickly, under a CPU
	// limit far too tight to run 2^40 of anything.
	'huge range on a near-empty table, different destination' => [ '
		local t, d = { 1, 2 }, {}
		table.move( t, 1, 2^40, 1, d )
		return d[1] .. "," .. d[2]
	', 'ok: \'1,2\'', 0.25 ],
	'huge range on a near-empty table, same table' => [ '
		local t = { 1, 2 }
		table.move( t, 1, 2^40, 1 )
		return t[1] .. "," .. t[2]
	', 'ok: \'1,2\'', 0.25 ],
];

foreach ( $cases as $label => $case ) {
	[ $code, $expected ] = $case;
	$result = run( $label, $code, $case[2] ?? 1 );
	printf( "%-46s %s\n", $label, $result === $expected ? 'ok' : "FAIL: $result" );
}

--EXPECT--
basic copy                                     ok
overlap, destination after source              ok
overlap, destination before source             ok
default destination is the source table        ok
returns the destination table                  ok
empty range leaves both tables untouched       ok
non-participating keys are untouched           ok
non-table source errors                        ok
huge range on a near-empty table, different destination ok
huge range on a near-empty table, same table   ok
