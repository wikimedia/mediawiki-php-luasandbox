--TEST--
PHP throwing exceptions to be caught by pcall()
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

	function hang_test(f)
		pcall_test(f)
		while true do end
	end
LUA;

function runtime_error() {
	throw new LuaSandboxRuntimeError("runtime error");
}
function fatal_error() {
	throw new LuaSandboxFatalError("fatal error");
}
function plain_exception() {
	throw new Exception("exception");
}

$tests = array(
	'Runtime error' => array( 'pcall_test', 'runtime_error' ),
	'Fatal error' => array( 'hang_test', 'fatal_error' ),
	'Plain Exception' => array( 'hang_test', 'plain_exception' ),
);

foreach ( $tests as $desc => $info ) {
	list( $wrapper, $funcName ) = $info;
	echo "$desc: ";
	try {
		$sandbox = new LuaSandbox;
		$sandbox->loadString( $lua )->call();
		$sandbox->setCPULimit( 0.25 );
		$sandbox->registerLibrary( 'test', array( 'test' => $funcName ) );
		$res = $sandbox->loadString( 'return test.test' )->call();
		print implode("\n",
			$sandbox->callFunction( $wrapper, $res[0] ) ) . "\n";
	} catch ( Exception $e ) {
		echo get_class( $e ) . ': ' . $e->getMessage() . "\n";
	}
}

--EXPECT--
Runtime error: Caught: runtime error
Fatal error: LuaSandboxFatalError: fatal error
Plain Exception: Exception: exception
