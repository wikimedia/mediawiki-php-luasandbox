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

// Blocked on every Lua version; see the omissions listed in library.c.
foreach ( [ 'create' ] as $name ) {
	$type = memberType( $name );
	printf( "%-14s %s\n", "table.$name", $type === 'nil' ? 'blocked' : "FAIL: $type" );
}

// Available on every Lua version LuaSandbox supports. move is LuaSandbox's
// own implementation rather than a pass-through of the native one, which
// isn't safe to expose as-is; see table-move.phpt.
foreach ( [ 'concat', 'insert', 'move', 'remove', 'sort' ] as $name ) {
	$type = memberType( $name );
	printf( "%-14s %s\n", "table.$name", $type === 'function' ? 'ok' : "FAIL: $type" );
}

--EXPECT--
table.create   blocked
table.concat   ok
table.insert   ok
table.move     ok
table.remove   ok
table.sort     ok
