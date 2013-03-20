$(srcdir)/luasandbox_version.h: $(srcdir)/debian/changelog
	$(AWK) '/^php-luasandbox \(([^)]+)\)/ { \
			print "#define LUASANDBOX_VERSION \"" substr( $$2, 2, length( $$2 ) - 2 ) "\""; exit \
		} \
		{ print "Cannot find version in debian/changelog" > "/dev/stderr"; exit 1 } \
		' < $(srcdir)/debian/changelog > $(srcdir)/luasandbox_version.h

$(builddir)/luasandbox.lo: $(srcdir)/luasandbox_version.h
