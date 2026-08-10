--TEST--
setfenv/getfenv, native on Lua 5.1 and reimplemented on Lua 5.2+
--FILE--
<?php

$sandbox = new LuaSandbox;
$sandbox->setMemoryLimit( 1000000 );
$sandbox->setCPULimit( 1 );

// Lua 5.2 removed setfenv/getfenv in favour of the _ENV upvalue; LuaSandbox
// reimplements them on top of it for later versions. That reimplementation
// can't distinguish "no _ENV upvalue" (a function that never refers to a
// global) from "not a function", so it errors in a couple of cases where
// native Lua 5.1 would silently succeed; see the version-dependent cases
// below and the comment on luasandbox_base_setfenv() in library.c.
preg_match( '/(\d+\.\d+)/', LuaSandbox::getVersionInfo()['Lua'], $m );
$has52 = version_compare( $m[1], '5.2', '>=' );

function run( $sandbox, $name, $code ) {
	try {
		$ret = $sandbox->loadString( $code, $name )->call();
		return 'ok: ' . var_export( $ret[0] ?? null, true );
	} catch ( LuaSandboxError $e ) {
		return 'error: ' . $e->getMessage();
	}
}

printf( "%-24s %s\n", 'getfenv(1)', run( $sandbox, 'getfenv1', '
	return getfenv( 1 ) == _G
' ) );

printf( "%-24s %s\n", 'getfenv(0)', run( $sandbox, 'getfenv0', '
	return getfenv( 0 ) == _G
' ) );

printf( "%-24s %s\n", 'getfenv/setfenv(func)', run( $sandbox, 'setfenv-func', '
	local function target() return x end
	setfenv( target, { x = 42 } )
	return target()
' ) );

printf( "%-24s %s\n", 'setfenv(level)', run( $sandbox, 'setfenv-level', '
	local function level1()
		setfenv( 1, { x = 99 } )
		return x
	end
	return level1()
' ) );

printf( "%-24s %s\n", 'setfenv(cfunction)', run( $sandbox, 'setfenv-cfunc', '
	setfenv( tostring, {} )
' ) );

printf( "%-24s %s\n", 'setfenv(non-function)', run( $sandbox, 'setfenv-nonfunc', '
	setfenv( {}, {} )
' ) );

// Tail calls elide the caller's stack frame, so the level that would have
// been the caller no longer resolves to a function -- this is the one piece
// of the original Lua 5.1 numbering that could not be reused as-is on 5.2+;
// LuaSandbox reconstructs it (see luasandbox_compat_getfunc() in library.c).
printf( "%-24s %s\n", 'getfenv via tail call', run( $sandbox, 'getfenv-tailcall', '
	local function foo()
		return getfenv( 2 )
	end
	local function bar()
		return foo()
	end
	local ok, err = pcall( bar )
	if ok then
		return "no error"
	end
	return ( err:gsub( "^.-:%d+: ", "" ):gsub( " at level %d+$", "" ) )
' ) );

// A function that never refers to a global has no _ENV upvalue to read on
// Lua 5.2+, so LuaSandbox falls back to the sandbox's globals for getfenv();
// setfenv() has nothing to overwrite, so it errors instead of silently
// discarding the requested environment. Lua 5.1 always has a real per-
// closure environment slot, so both calls succeed there.
printf( "%-24s %s\n", 'getfenv(no-globals func)', run( $sandbox, 'getfenv-noenv', '
	local function noenv() end
	return getfenv( noenv ) == _G
' ) );
$result = run( $sandbox, 'setfenv-noenv', '
	local function noenv() end
	setfenv( noenv, {} )
	return "ok"
' );
$expectSuccess = !$has52;
$actualSuccess = str_starts_with( $result, 'ok' );
printf( "%-24s %s\n", 'setfenv(no-globals func)',
	$actualSuccess === $expectSuccess ? 'ok' : "FAIL: $result" );

// setfenv(0, ...) replaces the running thread's global environment table.
// On Lua 5.1 that's a real operation LuaSandbox exposes as-is; on 5.2+
// there's no equivalent to redirect, since _ENV is resolved per-closure at
// load time, not looked up through the thread. Run this in its own sandbox
// since on 5.1 it really does replace the sandbox's global table.
$freshSandbox = new LuaSandbox;
$freshSandbox->setMemoryLimit( 1000000 );
$freshSandbox->setCPULimit( 1 );
$result = run( $freshSandbox, 'setfenv0', '
	setfenv( 0, {} )
	return "ok"
' );
$actualSuccess = str_starts_with( $result, 'ok' );
printf( "%-24s %s\n", 'setfenv(0, ...)',
	$actualSuccess === $expectSuccess ? 'ok' : "FAIL: $result" );

--EXPECT--
getfenv(1)               ok: true
getfenv(0)               ok: true
getfenv/setfenv(func)    ok: 42
setfenv(level)           ok: 99
setfenv(cfunction)       error: [string "setfenv-cfunc"]:2: 'setfenv' cannot change environment of given object
setfenv(non-function)    error: [string "setfenv-nonfunc"]:2: bad argument #1 to 'setfenv' (number expected, got table)
getfenv via tail call    ok: 'no function environment for tail call'
getfenv(no-globals func) ok: true
setfenv(no-globals func) ok
setfenv(0, ...)          ok
