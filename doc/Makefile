#----------------------------------------------------------------------------
#
# PostgreSQL documentation installation makefile
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/doc/Makefile,v 1.19 2000/09/17 13:02:28 petere Exp $
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


MODULES := admin postgres programmer tutorial user

found_MODULES := $(foreach module, $(MODULES), $(shell test -f $(srcdir)/$(module).tar.gz && echo $(module)))

ifneq ($(wildcard $(srcdir)/man.tar.gz),)
found_man := yes
endif


all:


install: all installdirs
ifneq ($(strip $(found_MODULES)),)
	for module in $(found_MODULES); do \
	  gzip -d -c $${module}.tar.gz | ( cd $(DESTDIR)$(docdir)/$$module && $(TAR) xf - ) || \
	  exit; \
	done
endif
ifdef found_man
	gzip -d -c man.tar.gz | ( cd $(DESTDIR)$(mandir) && $(TAR) xf - )
endif


installdirs:
	$(mkinstalldirs) $(DESTDIR)$(mandir) $(addprefix $(DESTDIR)$(docdir)/, . $(found_MODULES))


uninstall:
	-rm -rf $(addprefix $(DESTDIR)$(docdir)/, $(MODULES))
ifdef found_man
	-rm -f $(addprefix $(DESTDIR)$(mandir)/, $(shell gunzip -c man.tar.gz | tar tf -))
endif
