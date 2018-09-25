--TEST--
Extending LuaSandbox
--FILE--
<?php
// bugs T59292 and T205370
class ExtendedLuaSandbox extends LuaSandbox {
	public $var1;
	public $var2;
	public $var3;
	public $var4;
	public $var5;
}
$sandbox = new ExtendedLuaSandbox;

for($i=1; $i<=5; $i++){
	$sandbox->{"var$i"} = $i;
}
var_dump( $sandbox );

for($i=6; $i<=10; $i++){
	$sandbox->{"var$i"} = $i;
}
var_dump( $sandbox );

echo "ok\n";

// HHVM leaks it otherwise, and the warning makes the test fail
unset( $sandbox );

--EXPECT--
object(ExtendedLuaSandbox)#1 (5) {
  ["var1"]=>
  int(1)
  ["var2"]=>
  int(2)
  ["var3"]=>
  int(3)
  ["var4"]=>
  int(4)
  ["var5"]=>
  int(5)
}
object(ExtendedLuaSandbox)#1 (10) {
  ["var1"]=>
  int(1)
  ["var2"]=>
  int(2)
  ["var3"]=>
  int(3)
  ["var4"]=>
  int(4)
  ["var5"]=>
  int(5)
  ["var6"]=>
  int(6)
  ["var7"]=>
  int(7)
  ["var8"]=>
  int(8)
  ["var9"]=>
  int(9)
  ["var10"]=>
  int(10)
}
ok
