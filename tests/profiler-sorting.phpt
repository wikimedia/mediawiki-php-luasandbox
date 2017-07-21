--TEST--
profiler sorting
--FILE--
<?php

// Note these tests have to busy-loop. Even if Lua had an "os.sleep", it'd just
// say "sleep" used all the time. And we can't directly loop on os.clock() here
// either, because that would say "clock" used most of the time.

$lua = <<<LUA
	function test1()
		for i = 0, 1e6 do end
	end

	function test2()
		for i = 0, 4e6 do end
	end

	function test3()
		for i = 0, 2e6 do end
	end

	function test()
		local t = os.clock() + 0.5
		while os.clock() < t do
			test1()
			test2()
			test3()
		end
	end
LUA;

$sandbox = new LuaSandbox;
$sandbox->loadString( $lua )->call();
$sandbox->enableProfiler( 0.01 );

$sandbox->callFunction( 'test' );

foreach( [
	'samples' => LuaSandbox::SAMPLES,
	'seconds' => LuaSandbox::SECONDS,
	'percent' => LuaSandbox::PERCENT
] as $name => $stat ) {
	$result = $sandbox->getProfilerFunctionReport( $stat );
	// "clone" and sort
	$sorted = array_combine( array_keys( $result ), array_values( $result ) );
	arsort( $sorted );
	if ( $result === $sorted ) {
		echo "$name: OK\n";
	} else {
		echo "$name: FAIL\n";
		var_export( [
			'result' => $result,
			'sorted' => $sorted,
		] );
	}
}

// HHVM leaks it otherwise, and the warning makes the test fail
unset( $sandbox );

--EXPECTF--
samples: OK
seconds: OK
percent: OK
