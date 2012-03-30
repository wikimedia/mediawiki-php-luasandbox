--TEST--
xpcall() basic behaviour
--FILE--
<?php

$lua = <<<LUA
	function xpcall_test(f, err)
		local status, msg
		status, msg = xpcall(f, err)
		if not status then
			return msg
		else
			return "success"
		end
	end
LUA;

$xperr = 'return "xp: " .. msg';

$tests = array(
	'Normal' => array(
		'return 1',
		$xperr
	),
	'User error' => array(
		'error("runtime error")',
		$xperr
	),
	'Error in error handler' => array(
		'error("original error")',
		'error("error in handler")'
	),
	'Unconvertible error in error handler' => array(
		'error("original error")',
		'error({})'
	),
	'Numeric error in error handler' => array(
		'error("original error")',
		'error(2)',
	),
	'Argument check error' => array(
		'string.byte()',
		$xperr
	),
	'Protected infinite recursion' => array(
		'function foo() foo() end foo()',
		$xperr
	),
	'Infinite recursion in handler' => array(
		'error("x")',
		'function foo() foo() end foo()'
	),
	'Protected infinite loop' => array(
		'while true do end',
		$xperr,
	),
	'Infinite loop in handler' => array(
		'error("x")',
		'while true do end',
	),		
	'Out of memory in handler' => array(
		'error("x")',
		'string.rep("x", 1000000)'
	),
);

$sandbox = new LuaSandbox;
$sandbox->loadString( $lua )->call();
$sandbox->setCPULimit( 0.25 );
$sandbox->setMemoryLimit( 100000 );

foreach ( $tests as $desc => $info ) {
	$sandbox = new LuaSandbox;
	$sandbox->loadString( $lua )->call();
	$sandbox->setCPULimit( 0.25 );
	$sandbox->setMemoryLimit( 100000 );
	echo "$desc: ";
	list( $code, $errorCode ) = $info;
	$func = $sandbox->loadString( $code );
	$errorCode = "return function(msg) $errorCode end";
	$ret = $sandbox->loadString( $errorCode )->call();
	$errorFunc = $ret[0];
	
	try {
		print implode("\n", 
			$sandbox->callFunction( 'xpcall_test', $func, $errorFunc ) ) . "\n";
	} catch ( LuaSandboxError $e ) {
		echo "LuaSandboxError: " . $e->getMessage() . "\n";
	}
}
--EXPECT--
Normal: success
User error: xp: [string ""]:1: runtime error
Error in error handler: LuaSandboxError: [string ""]:1: error in handler
Unconvertible error in error handler: LuaSandboxError: unknown error
Numeric error in error handler: LuaSandboxError: [string ""]:1: 2
Argument check error: xp: [string ""]:1: bad argument #1 to 'byte' (string expected, got no value)
Protected infinite recursion: LuaSandboxError: not enough memory
Infinite recursion in handler: LuaSandboxError: not enough memory
Protected infinite loop: LuaSandboxError: The maximum execution time for this script was exceeded
Infinite loop in handler: LuaSandboxError: The maximum execution time for this script was exceeded
Out of memory in handler: LuaSandboxError: not enough memory
