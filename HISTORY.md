## 4.1.4 (2026-07-30)
* Migrate from PECL to PIE ([T423562](https://phabricator.wikimedia.org/T423562))

## 4.1.3 (2025-12-15)
* Add PHP 8.5 support (Patch by Remi Collet)
* Fix segfault in unpack

## 4.1.2 (2023-12-13)
* Run the GC more aggressively, especially as usage approaches the limit ([T349462](https://phabricator.wikimedia.org/T349462))

## 4.1.1 (2023-07-31)
* Fix segmentation fault when memory limit is exceeded in LuaSandbox init
* Fix incorrect version reported by `phpversion('luasandbox')`

## 4.1.0 (2022-09-23)
* Add PHP 8.2 support

## 4.0.2 (2021-05-19)
* Add config.w32 package.xml tarball ([#80850](https://bugs.php.net/bug.php?id=80850))

## 4.0.1 (2021-03-10)
* Add missing file to package.xml tarball

## 4.0.0 (2021-03-04)
* Add docbook documentation for php.net
* Flag optional and variadic parameters properly for PHP reflection
* Remove memory leaks in `data_conversion.c`
* Remove PHP5 and HHVM compatibility
* Add PHP 8 support
* Fix Windows compilation

## 3.0.3 (2018-10-11)
* Fix ZTS build on PHP 7+ (Patch by Remi Collet)

## 3.0.2 (2018-10-09)
* Fix PHP 7 object layout
* Initial PECL release
