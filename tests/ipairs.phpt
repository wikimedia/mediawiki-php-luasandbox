--TEST--
ipairs() and __ipairs
--FILE--
<?php
$lua = <<<LUA
	function ipairs_test1()
		local t = { 'a' }
		local f, s, var = ipairs( t )
		if type( f ) == 'function' and s == t and var == 0 then
			local k, v = f(t, var)
			if k == 1 and v == 'a' then
				return "Ok"
			else
				return "Fail: First call returned " .. k .. "\\t" .. v
			end
		else
			return "Fail:\\n" ..
				tostring(f) .. '\\t' .. tostring(s) .. '\\t' .. tostring(var) .. '\\n' ..
				tostring(next) .. '\\t' .. tostring(t) .. '\\t0'
		end
	end

	function ipairs_test2()
		local t = { 1 }
		setmetatable( t, { __ipairs = function () return 1, 2, 3 end } )
		local f, s, var = ipairs( t )
		if f == 1 and s == 2 and var == 3 then
			return "Ok"
		else
			return "Fail:\\n" ..
				tostring(f) .. '\\t' .. tostring(s) .. '\\t' .. tostring(var) .. '\\n' ..
				'1\\t2\\t3'
		end
	end

	function ipairs_test3()
		ipairs()
		return "Fail: Should have thrown an error"
	end
LUA;

$tests = array(
	'Normal' => 'ipairs_test1',
	'With __ipairs' => 'ipairs_test2',
	'No argument' => 'ipairs_test3',
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
With __ipairs: Ok
No argument: LuaSandboxError: [string ""]:32: bad argument #1 to 'ipairs' (table expected, got no value)
