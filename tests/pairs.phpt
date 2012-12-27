--TEST--
pairs() and __pairs
--FILE--
<?php
$lua = <<<LUA
	function pairs_test1()
		local t = { a = 1 }
		local f, s, var = pairs( t )
		if type( f ) == 'function' and s == t and var == nil then
			local k, v = f(t, var)
			if k == 'a' and v == 1 then
				return "Ok"
			else
				return "Fail: First call returned " .. k .. "\\t" .. v
			end
		else
			return "Fail:\\n" ..
				tostring(f) .. '\\t' .. tostring(s) .. '\\t' .. tostring(var) .. '\\n' ..
				tostring(next) .. '\\t' .. tostring(t) .. '\\tnil'
		end
	end

	function pairs_test2()
		local t = { a = 1 }
		setmetatable( t, { __pairs = function () return 1, 2, 3 end } )
		local f, s, var = pairs( t )
		if f == 1 and s == 2 and var == 3 then
			return "Ok"
		else
			return "Fail:\\n" ..
				tostring(f) .. '\\t' .. tostring(s) .. '\\t' .. tostring(var) .. '\\n' ..
				'1\\t2\\t3'
		end
	end

	function pairs_test3()
		pairs()
		return "Fail: Should have thrown an error"
	end
LUA;

$tests = array(
	'Normal' => 'pairs_test1',
	'With __pairs' => 'pairs_test2',
	'No argument' => 'pairs_test3',
);

foreach ( $tests as $desc => $func ) {
	echo "$desc: ";
	$sandbox = new LuaSandbox;
	$sandbox->loadString( $lua )->call();
	$sandbox->setCPULimit( 0.25 );
	$sandbox->setMemoryLimit( 100000 );
	try {
		print implode("\n", $sandbox->callFunction( $func ) ) . "\n";
	} catch ( LuaSandboxError $e ) {
		echo "LuaSandboxError: " . $e->getMessage() . "\n";
	}
}
--EXPECT--
Normal: Ok
With __pairs: Ok
No argument: LuaSandboxError: [string ""]:32: bad argument #1 to 'pairs' (table expected, got no value)
