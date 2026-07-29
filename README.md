LuaSandbox is an extension for PHP 7 and PHP 8 to allow safely
running untrusted Lua 5.1 code from within PHP, which will generally
be faster than shelling out to a Lua binary and using inter-process
communication.

This extension has documentation in three different formats. When updating
the documentation in one place, remember to update the others as well.

## On-wiki documentation

Installation of the extension and some examples are documented
[on mediawiki.org](https://www.mediawiki.org/wiki/LuaSandbox).

## PHPdoc style documentation

PHP interface documentation is provided via stub class definitions in
the stubs/ directory. These stubs may be used with IDEs that understand
PHPdoc documentation.

These stubs, along with a brief introduction in README.md, may be used
to generate online documentation using Doxygen. It should suffice to run
`doxygen` with no arguments.

## PHP DocBook documentation

Documentation in DocBook format is included in the
[LuaSandbox chapter](https://www.php.net/manual/en/book.luasandbox.php) in the
PHP manual.

To update this manual chapter, submit a pull request to
https://github.com/php/doc-en

