#----------------------------------------------------------------------------
#
# PostgreSQL documentation top-level makefile
#
# Copyright (c) 1994, Regents of the University of California
#
# doc/Makefile
#
#----------------------------------------------------------------------------

subdir = doc
top_builddir = ..
include $(top_builddir)/src/Makefile.global

all distprep html man install installdirs uninstall clean distclean maintainer-clean:
	$(MAKE) -C src $@
