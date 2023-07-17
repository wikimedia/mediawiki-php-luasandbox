--TEST--
Memory limit exceeded during sandbox init
--INI--
memory_limit=2M
--FILE--
<?php
$buf = str_repeat('a', 1000000);
$sandboxes = [];
for ($i = 0; $i < 100; $i++) {
    $sandboxes[] = new LuaSandbox();
}
?>
--EXPECTF--
Fatal error: Allowed memory size of 2097152 bytes exhausted%s(tried to allocate %d bytes) in %s on line %d
