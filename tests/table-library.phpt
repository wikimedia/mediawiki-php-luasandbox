--TEST--
Table library members exposed to the sandbox
--FILE--
<?php

$sandbox = new LuaSandbox;
$sandbox->setMemoryLimit( 1000000 );
$sandbox->setCPULimit( 1 );

function memberType( $name ) {
	global $sandbox;
	return $sandbox->loadString( "return type(table.$name)" )->call()[0];
}

// Blocked on every Lua version. table.move runs its copy entirely inside a C
// loop, so neither the instruction count hook nor the memory limit can
// interrupt it; see the omissions listed in library.c.
foreach ( [ 'move', 'create' ] as $name ) {
	$type = memberType( $name );
	printf( "%-14s %s\n", "table.$name", $type === 'nil' ? 'blocked' : "FAIL: $type" );
}

// Available on every Lua version LuaSandbox supports
foreach ( [ 'concat', 'insert', 'remove', 'sort' ] as $name ) {
	$type = memberType( $name );
	printf( "%-14s %s\n", "table.$name", $type === 'function' ? 'ok' : "FAIL: $type" );
}

// Calling a blocked member is an ordinary Lua error, not a sandbox escape
$sandbox->setCPULimit( 0.25 );
try {
	$sandbox->loadString( 'local t = {1, 2} table.move( t, 1, 2^40, 1, {} )' )->call();
	print "table.move call FAIL: no error thrown\n";
} catch ( LuaSandboxRuntimeError $e ) {
	printf( "%-14s %s\n", 'table.move call',
		str_contains( $e->getMessage(), 'nil value' ) ? 'rejected' : 'FAIL: ' . $e->getMessage() );
}

--EXPECT--
table.move     blocked
table.create   blocked
table.concat   ok
table.insert   ok
table.remove   ok
table.sort     ok
table.move call rejected
