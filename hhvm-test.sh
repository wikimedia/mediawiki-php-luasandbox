#!/bin/sh
# First run `phpize`, and then ./hhvm-test.sh run-tests.php
export TEST_PHP_EXECUTABLE=$0
hhvm -d "hhvm.dynamic_extensions[luasandbox]=$(pwd)/luasandbox.so" "$@"
