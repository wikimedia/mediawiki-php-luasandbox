<?php

/**
 * Represents a Lua function, allowing it to be called from PHP.
 *
 * A LuaSandboxFunction may be obtained as a return value from Lua, as a parameter
 * passed to a callback from Lua, or by using LuaSandbox::wrapPhpFunction(),
 * LuaSandbox::loadString(), or LuaSandbox::loadBinary().
 */
class LuaSandboxFunction {

	final private function __construct() {
	}

	/**
	 * Call a Lua function.
	 *
	 * Errors considered to be the fault of the PHP code will result in the
	 * function returning false and E_WARNING being raised, for example, a
	 * resource type being used as an argument. Lua errors will result in a
	 * LuaSandboxRuntimeError exception being thrown.
	 *
	 * PHP and Lua types are converted as follows:
	 *  - PHP null is Lua nil, and vice versa
	 *  - PHP integers and floats are converted to Lua numbers. Infinity and
	 *    NAN are supported.
	 *  - Lua numbers without a fractional part between approximately -2**53
	 *    and 2**53 are converted to PHP integers, with others being converted
	 *    to PHP floats.
	 *  - PHP booleans are Lua booleans, and vice versa.
	 *  - PHP strings are Lua strings, and vice versa.
	 *  - Lua functions are PHP LuaSandboxFunction objects, and vice versa.
	 *    General PHP callables are not supported.
	 *  - PHP arrays are converted to Lua tables, and vice versa.
	 *    - Note that Lua typically indexes arrays from 1, while PHP indexes
	 *      arrays from 0. No adjustment is made for these differing
	 *      conventions.
	 *    - Self-referenial arrays are not supported in either direction.
	 *    - PHP references are dereferenced.
	 *    - Lua `__pairs` and `__ipairs` are processed. `__index` is ignored.
	 *    - When converting from PHP to Lua, integer keys between -2**53 and
	 *      2**53 are represented as Lua numbers. All other keys are represented
	 *      as Lua strings.
	 *    - When converting from Lua to PHP, keys other than strings and
	 *      numbers will result in an error, as will collisions when converting
	 *      numbers to strings or vice versa (since PHP considers things like
	 *      `$a[0]` and `$a["0"]` as being equivalent).
	 *  - All other types are unsupported and will raise an error/exception,
	 *    including general PHP objects and Lua userdata and thread types.
	 *
	 * Lua functions inherently return a list of results. So on success, this
	 * method returns an array containing all of the values returned by Lua,
	 * with integer keys starting from zero. Lua may return no results, in
	 * which case an empty array is returned.
	 *
	 * @param mixed $args,... Arguments passed to the function.
	 * @return array|false Return values from the function.
	 */
	public function call( /*...*/ ) {
	}

	/**
	 * Dump the function as a binary blob
	 * @return string To be passed to LuaSandbox::loadBinary()
	 */
	public function dump() {
	}
}
