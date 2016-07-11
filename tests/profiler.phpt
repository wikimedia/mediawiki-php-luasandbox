--TEST--
profiler
--FILE--
<?php

// Note these tests have to waste CPU cycles rather than sleep(), because the
// timer counts CPU time used and sleep() doesn't use CPU time.

$lua = <<<LUA
	lua = {}

	function lua.test()
		local t = os.clock() + 0.2
		while os.clock() < t do end
	end
LUA;

$sandbox = new LuaSandbox;
$sandbox->loadString( $lua )->call();
$sandbox->enableProfiler( 0.02 );

$sandbox->callFunction( 'lua.test' );

echo "Samples: " . $sandbox->getProfilerFunctionReport( LuaSandbox::SAMPLES )['clock'] . "\n";
echo "Seconds: " . $sandbox->getProfilerFunctionReport( LuaSandbox::SECONDS )['clock'] . "\n";
echo "Percent: " . $sandbox->getProfilerFunctionReport( LuaSandbox::PERCENT )['clock'] . "\n";

// Test that re-enabling the profiler doesn't explode
$sandbox->enableProfiler( 0.03 );

$sandbox->callFunction( 'lua.test' );

echo "Samples: " . $sandbox->getProfilerFunctionReport( LuaSandbox::SAMPLES )['clock'] . "\n";
echo "Seconds: " . $sandbox->getProfilerFunctionReport( LuaSandbox::SECONDS )['clock'] . "\n";
echo "Percent: " . $sandbox->getProfilerFunctionReport( LuaSandbox::PERCENT )['clock'] . "\n";

// Test that disabling the profiler doesn't explode
$sandbox->disableProfiler();

// HHVM leaks it otherwise, and the warning makes the test fail
unset( $sandbox );

--EXPECTF--
Samples: %d
Seconds: %f
Percent: %f
Samples: %d
Seconds: %f
Percent: %f
