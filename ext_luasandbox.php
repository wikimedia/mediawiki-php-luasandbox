<?hh

<<__NativeData("ZendCompat")>> class LuaSandbox {
	<<__Native("ZendCompat")>> public static function getVersionInfo(): mixed;
	<<__Native("ZendCompat")>> function loadString(mixed $code, mixed $chunkName): mixed;
	<<__Native("ZendCompat")>> function loadBinary(mixed $code, mixed $chunkName): mixed;
	<<__Native("ZendCompat")>> function setMemoryLimit(mixed $limit): mixed;
	<<__Native("ZendCompat")>> function getMemoryUsage(): mixed;
	<<__Native("ZendCompat")>> function getPeakMemoryUsage(): mixed;
	<<__Native("ZendCompat")>> function setCPULimit(mixed $normal_limit, mixed $emergency_limit): mixed;
	<<__Native("ZendCompat")>> function getCPUUsage(): mixed;
	<<__Native("ZendCompat")>> function pauseUsageTimer(): mixed;
	<<__Native("ZendCompat")>> function unpauseUsageTimer(): mixed;
	<<__Native("ZendCompat")>> function enableProfiler(mixed $period): mixed;
	<<__Native("ZendCompat")>> function disableProfiler(): mixed;
	<<__Native("ZendCompat")>> function getProfilerFunctionReport(mixed $units): mixed;
	<<__Native("ZendCompat")>> function callFunction(mixed $name): mixed;
	<<__Native("ZendCompat")>> function wrapPhpFunction(mixed $name, mixed $function): mixed;
	<<__Native("ZendCompat")>> function registerLibrary(mixed $libname, mixed $functions): mixed;
}

<<__NativeData("ZendCompat")>> class LuaSandboxError extends Exception {}
<<__NativeData("ZendCompat")>> class LuaSandboxRuntimeError extends LuaSandboxError {}
<<__NativeData("ZendCompat")>> class LuaSandboxFatalError extends LuaSandboxError {}
<<__NativeData("ZendCompat")>> class LuaSandboxSyntaxError extends LuaSandboxError {}
<<__NativeData("ZendCompat")>> class LuaSandboxMemoryError extends LuaSandboxError {}
<<__NativeData("ZendCompat")>> class LuaSandboxErrorError extends LuaSandboxError {}
<<__NativeData("ZendCompat")>> class LuaSandboxTimeoutError extends LuaSandboxError {}
<<__NativeData("ZendCompat")>> class LuaSandboxEmergencyTimeoutError extends LuaSandboxError {}

<<__NativeData("ZendCompat")>> class LuaSandboxFunction {
	<<__Native("ZendCompat")>> private final function __construct(): mixed;
	<<__Native("ZendCompat")>> function call(): mixed;
	<<__Native("ZendCompat")>> function dump(): mixed;
}

