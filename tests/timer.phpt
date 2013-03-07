--TEST--
timer pausing and unpausing
--FILE--
<?php

// Note these tests have to waste CPU cycles rather than sleep(), because the
// timer counts CPU time used and sleep() doesn't use CPU time.

$lua = <<<LUA
	lua = {}

	function lua.expensive()
		local t = os.clock() + 0.2
		while os.clock() < t do end
	end

	function test_auto_unpause()
		php.paused();
		lua.expensive();
	end

	function test_pause_overrun()
		php.overrun();
		local t = os.clock() + 0.2
		while os.clock() < t do end
	end

	function lua.call( func, ... )
		func( ... )
	end
LUA;

function expensive() {
	$t = microtime( 1 ) + 0.2;
	while ( microtime( 1 ) < $t ) {
	}
}

function paused() {
	global $sandbox;
	$sandbox->pauseUsageTimer();
	$t = microtime( 1 ) + 0.2;
	while ( microtime( 1 ) < $t ) {
	}
}

function unpaused() {
	global $sandbox;
	$sandbox->pauseUsageTimer();
	$sandbox->unpauseUsageTimer();
	$t = microtime( 1 ) + 0.2;
	while ( microtime( 1 ) < $t ) {
	}
}

function overrun() {
	global $sandbox;
	$sandbox->pauseUsageTimer();
	$t = microtime( 1 ) + 0.45;
	while ( microtime( 1 ) < $t ) {
	}
}

function resetLimit() {
	global $sandbox;
	$sandbox->pauseUsageTimer();
	$sandbox->setCPULimit( 0.1 );
	$t = microtime( 1 ) + 0.2;
	while ( microtime( 1 ) < $t ) {
	}
}

function call() {
	global $sandbox;
	$args = func_get_args();
	call_user_func_array( array( $sandbox, 'callFunction' ), $args );
}

function pauseCall() {
	global $sandbox;
	$sandbox->pauseUsageTimer();
	$args = func_get_args();
	call_user_func_array( array( $sandbox, 'callFunction' ), $args );
}

function doTest( $name ) {
	global $sandbox, $lua;

	$args = func_get_args();
	array_shift( $args );

	printf( "%-47s ", "$name:" );

	$sandbox = new LuaSandbox;
	$sandbox->registerLibrary( 'php', array(
		'expensive' => 'expensive',
		'paused' => 'paused',
		'unpaused' => 'unpaused',
		'overrun' => 'overrun',
		'resetLimit' => 'resetLimit',
		'call' => 'call',
		'pauseCall' => 'pauseCall',
	) );
	$sandbox->loadString( $lua )->call();
	$sandbox->setMemoryLimit( 100000 );
	$sandbox->setCPULimit( 0.1 );

	try {
		$timeout='no';
		$t0 = microtime( 1 );
		$u0 = $sandbox->getCPUUsage();
		call_user_func_array( array( $sandbox, 'callFunction' ), $args );
	} catch ( LuaSandboxTimeoutError $err ) {
		$timeout='yes';
	}
	$t1 = microtime( 1 ) - $t0;
	$u1 = $sandbox->getCPUUsage() - $u0;
	printf( "%3s (%.1fs of %.1fs)\n", $timeout, $u1, $t1 );
}

doTest( 'Lua usage counted', 'lua.expensive' );
doTest( 'PHP usage counted', 'php.expensive' );
doTest( 'Paused PHP usage counted', 'php.paused' );
doTest( 'Unpause works', 'php.unpaused' );
doTest( 'Auto-unpause works', 'test_auto_unpause' );
doTest( 'Reset limit unpauses', 'php.resetLimit' );
doTest( 'Pause overrun prevented', 'test_pause_overrun' );

doTest( 'PHP to Lua counted', 'php.call', 'lua.expensive' );
doTest( 'PHP to paused-PHP counted', 'php.call', 'php.paused' );
doTest( 'PHP to paused-PHP to paused-PHP counted', 'php.call', 'php.pauseCall', 'php.paused' );
doTest( 'paused-PHP to Lua counted', 'php.pauseCall', 'lua.expensive' );
doTest( 'paused-PHP to PHP counted', 'php.pauseCall', 'php.expensive' );
doTest( 'paused-PHP to paused-PHP counted', 'php.pauseCall', 'php.paused' );
doTest( 'paused-PHP to paused-PHP to paused-PHP counted', 'php.pauseCall', 'php.pauseCall', 'php.paused' );
doTest( 'paused-PHP to PHP to paused-PHP counted', 'php.pauseCall', 'php.call', 'php.paused' );

--EXPECTF--
Lua usage counted:                              yes (0.1s of %fs)
PHP usage counted:                              yes (0.2s of %fs)
Paused PHP usage counted:                        no (0.0s of %fs)
Unpause works:                                  yes (0.2s of %fs)
Auto-unpause works:                             yes (0.1s of %fs)
Reset limit unpauses:                            no (0.0s of %fs)
Pause overrun prevented:                        yes (0.1s of %fs)
PHP to Lua counted:                             yes (0.1s of %fs)
PHP to paused-PHP counted:                      yes (0.2s of %fs)
PHP to paused-PHP to paused-PHP counted:        yes (0.2s of %fs)
paused-PHP to Lua counted:                      yes (0.1s of %fs)
paused-PHP to PHP counted:                      yes (0.2s of %fs)
paused-PHP to paused-PHP counted:                no (0.0s of %fs)
paused-PHP to paused-PHP to paused-PHP counted:  no (0.0s of %fs)
paused-PHP to PHP to paused-PHP counted:        yes (0.2s of %fs)
