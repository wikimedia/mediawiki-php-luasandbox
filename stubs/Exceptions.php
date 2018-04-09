<?php

/**
 * Base class for LuaSandbox exceptions
 */
class LuaSandboxError extends Exception {
	const RUN = 2; // LUA_ERRRUN
	const SYNTAX = 3; // LUA_ERRSYNTAX
	const MEM = 4; // LUA_ERRMEM
	const ERR = 5; // LUA_ERRERR

	/**
	 * @var ?array
	 */
	public $luaTrace;
}

/**
 * Catchable LuaSandbox runtime exceptions
 *
 * These may be caught inside Lua using `pcall()` or `xpcall()`
 */
class LuaSandboxRuntimeError extends LuaSandboxError {
}

/**
 * Uncatchable LuaSandbox exceptions
 *
 * These may not be caught inside Lua using `pcall()` or `xpcall()`
 */
class LuaSandboxFatalError extends LuaSandboxError {
}

/**
 * Exception thrown when Lua code cannot be parsed
 */
class LuaSandboxSyntaxError extends LuaSandboxFatalError {
}

/**
 * Exception thrown when Lua cannot allocate memory
 * @see LuaSandbox::setMemoryLimit
 */
class LuaSandboxMemoryError extends LuaSandboxFatalError {
}

/**
 * Exception thrown when Lua encounters an error inside an error handler
 */
class LuaSandboxErrorError extends LuaSandboxFatalError {
}

/**
 * Exception thrown when the configured CPU time limit is exceeded
 * @see LuaSandbox::setCPULimit
 */
class LuaSandboxTimeoutError extends LuaSandboxFatalError {
}
