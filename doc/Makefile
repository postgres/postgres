#----------------------------------------------------------------------------
#
# PostgreSQL documentation installation makefile
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/doc/Makefile,v 1.20 2000/12/26 20:47:07 petere Exp $
#
#----------------------------------------------------------------------------

# This directory doesn't build anything, it is only responsible for
# installing the documenation. It is invoked automatically by the
# top-level makefile. The files to be installed are prepared specially
# and are placed in this directory during distribution bundling. In
# CVS-based trees these files don't exist, so we skip the installation
# in that case.
#
# To actually build the documenation, look into the src/ and src/sgml
# subdirectories.

subdir = doc
top_builddir = ..
include $(top_builddir)/src/Makefile.global


ifneq ($(wildcard $(srcdir)/postgres.tar.gz),)
found_html := yes
endif

ifneq ($(wildcard $(srcdir)/man.tar.gz),)
found_man := yes
endif


all:


install: all installdirs
ifdef found_html
	gzip -d -c postgres.tar.gz | ( cd $(DESTDIR)$(docdir)/html && $(TAR) xf - )
endif
ifdef found_man
	gzip -d -c man.tar.gz | ( cd $(DESTDIR)$(mandir) && $(TAR) xf - )
endif


installdirs:
	$(mkinstalldirs) $(DESTDIR)$(mandir) $(DESTDIR)$(docdir)/html


uninstall:
ifdef found_html
	-rm -f $(addprefix $(DESTDIR)$(docdir)/html/, $(shell gunzip -c $(srcdir)/postgres.tar.gz | tar tf -))
endif
ifdef found_man
	-rm -f $(addprefix $(DESTDIR)$(mandir)/, $(shell gunzip -c $(srcdir)/man.tar.gz | tar tf -))
endif
