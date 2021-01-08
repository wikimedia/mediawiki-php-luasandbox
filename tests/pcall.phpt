--TEST--
pcall() catching various errors
--FILE--
<?php
$lua = <<<LUA
	function pcall_test(f)
		local status, msg
		status, msg = pcall(f)
		if not status then
			return "Caught: " .. msg
		else
			return "success"
		end
	end
LUA;

$tests = array(
	'Normal' => 'return 1',
	'User error' => 'error("runtime error")',
	'Argument check error' => 'string.byte()',
	'Infinite recursion' => 'function foo() foo() end foo()',
	'Infinite loop (timeout)' => 'while true do end',
	'Out of memory' => 'string.rep("x", 1000000)'
);

foreach ( $tests as $desc => $code ) {
	echo "$desc: ";
	$sandbox = new LuaSandbox;
	$sandbox->loadString( $lua )->call();
	$sandbox->setCPULimit( 0.25 );
	$sandbox->setMemoryLimit( 100000 );
	try {
		print implode("\n",
			$sandbox->callFunction( 'pcall_test', $sandbox->loadString( $code ) ) ) . "\n";
	} catch ( LuaSandboxError $e ) {
		echo "LuaSandboxError: " . $e->getMessage() . "\n";
	}
}

--EXPECT--
Normal: success
User error: Caught: [string ""]:1: runtime error
Argument check error: Caught: [string ""]:1: bad argument #1 to 'byte' (string expected, got no value)
Infinite recursion: LuaSandboxError: not enough memory
Infinite loop (timeout): LuaSandboxError: The maximum execution time for this script was exceeded
Out of memory: LuaSandboxError: not enough memory
