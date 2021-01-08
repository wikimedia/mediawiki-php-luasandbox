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

	function pairs_return()
		local data = { a = 1, b = 2 }
		local t = {}
		setmetatable( t, { __pairs = function () return pairs( data ) end } )
		return t
	end

	function pairs_error()
		local t = {}
		setmetatable( t, { __pairs = function () error( "Error from __pairs function" ) end } )
		pairs( t )
		return "Fail: Should have thrown an error"
	end

	function pairs_return_error()
		local t = {}
		setmetatable( t, { __pairs = function () error( "Error from __pairs function" ) end } )
		return t
	end

	function pairs_next_error()
		local t = {}
		setmetatable( t, {
			__pairs = function ()
				return function() error( "Error from next function" ) end
			end
		} )
		for k, v in pairs( t ) do end
		return "Fail: Should have thrown an error"
	end

	function pairs_return_next_error()
		local t = {}
		setmetatable( t, {
			__pairs = function ()
				return function() error( "Error from next function" ) end
			end
		} )
		return t
	end
LUA;

$tests = array(
	'Normal' => 'pairs_test1',
	'With __pairs' => 'pairs_test2',
	'No argument' => 'pairs_test3',
	'With __pairs throwing an error' => 'pairs_error',
	'With next func throwing an error' => 'pairs_next_error',
	'Table with __pairs returned to PHP' => 'pairs_return',
	'Table with __pairs throwing an error returned to PHP' => 'pairs_return_error',
	'Table with next func throwing an error returned to PHP' => 'pairs_return_next_error',
);

foreach ( $tests as $desc => $func ) {
	echo "$desc: ";
	$sandbox = new LuaSandbox;
	$sandbox->loadString( $lua )->call();
	$sandbox->setCPULimit( 0.25 );
	$sandbox->setMemoryLimit( 100000 );
	try {
		print var_export( $sandbox->callFunction( $func ), 1 ) . "\n";
	} catch ( LuaSandboxError $e ) {
		echo "LuaSandboxError: " . $e->getMessage() . "\n";
	}
}

--EXPECT--
Normal: array (
  0 => 'Ok',
)
With __pairs: array (
  0 => 'Ok',
)
No argument: LuaSandboxError: [string ""]:32: bad argument #1 to 'pairs' (table expected, got no value)
With __pairs throwing an error: LuaSandboxError: [string ""]:45: Error from __pairs function
With next func throwing an error: LuaSandboxError: [string ""]:60: Error from next function
Table with __pairs returned to PHP: array (
  0 => 
  array (
    'a' => 1,
    'b' => 2,
  ),
)
Table with __pairs throwing an error returned to PHP: LuaSandboxError: [string ""]:52: Error from __pairs function
Table with next func throwing an error returned to PHP: LuaSandboxError: [string ""]:71: Error from next function
