--TEST--
Array key conversion
--FILE--
<?php

function testPhpToLua( $test, $array ) {
	printf( "PHP→Lua %-30s ", "$test:" );

	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	try {
		$ret = $sandbox
			->loadString( 'local t, r = ..., {}; for k, v in pairs( t ) do r[v] = type(k) end return r' )
			->call( $array );
		if ( is_array( $ret[0] ) ) {
			ksort( $ret[0], SORT_STRING );
		}
		printf( "%s\n", preg_replace( '/\s+/', ' ', var_export( $ret[0], 1 ) ) );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}
}

function testLuaToPhp( $test, $lua ) {
	printf( "Lua→PHP %-30s ", "$test:" );

	$sandbox = new LuaSandbox;
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );
	try {
		$ret = $sandbox->loadString( "return { $lua }" )->call();
		if ( is_array( $ret[0] ) ) {
			ksort( $ret[0], SORT_STRING );
		}
		printf( "%s\n", preg_replace( '/\s+/', ' ', var_export( $ret[0], 1 ) ) );
	} catch ( LuaSandboxError $e ) {
		printf( "EXCEPTION: %s\n", $e->getMessage() );
	}
}

if ( PHP_INT_MAX > 9007199254740992 ) {
	$a = [
		'9007199254740992' => 'max', '9007199254740993' => 'max+1',
		'-9007199254740992' => 'min', '-9007199254740993' => 'min-1',
	];
	$max = '9223372036854775807';
	$max2 = '9223372036854775808';
	$min = '-9223372036854775808';
	$min2 = '-9223372036854775809';
} else {
	$a = [
		'2147483647' => 'max', '2147483648' => 'max+1',
		'-2147483648' => 'min', '-2147483649' => 'min-1',
	];
	$max = '2147483647';
	$max2 = '2147483648';
	$min = '-2147483648';
	$min2 = '-2147483649';
}

testPhpToLua( 'simple integers', [ -10 => 'minus ten', 0 => 'zero', 10 => 'ten' ] );
testPhpToLua( 'maximal values', $a );

testLuaToPhp( 'simple integers', '[-10] = "minus ten", [0] = "zero", [10] = "ten"' );
testLuaToPhp( 'stringified integers', '["-10"] = "minus ten", ["0"] = "zero", ["10"] = "ten"' );
testLuaToPhp( 'maximal integers', "['$max'] = 'max', ['$max2'] = 'max+1', ['$min'] = 'min', ['$min2'] = 'min-1'" );
testLuaToPhp( 'collision (0)', '[0] = "number zero", ["0"] = "string zero"' );
testLuaToPhp( 'collision (float)', '[1.5] = "number 1.5", ["1.5"] = "string 1.5"' );
testLuaToPhp( 'collision (inf)', '[1/0] = "number inf", ["inf"] = "string inf"' );

--EXPECTF--
PHP→Lua simple integers:               array ( 'minus ten' => 'number', 'ten' => 'number', 'zero' => 'number', )
PHP→Lua maximal values:                array ( 'max' => 'number', 'max+1' => 'string', 'min' => 'number', 'min-1' => 'string', )
Lua→PHP simple integers:               array ( -10 => 'minus ten', 0 => 'zero', 10 => 'ten', )
Lua→PHP stringified integers:          array ( -10 => 'minus ten', 0 => 'zero', 10 => 'ten', )
Lua→PHP maximal integers:              array ( -%d => 'min', '-%d' => 'min-1', %d => 'max', '%d' => 'max+1', )
Lua→PHP collision (0):                 EXCEPTION: Collision for array key 0 when passing data from Lua to PHP
Lua→PHP collision (float):             EXCEPTION: Collision for array key 1.5 when passing data from Lua to PHP
Lua→PHP collision (inf):               EXCEPTION: Collision for array key inf when passing data from Lua to PHP
