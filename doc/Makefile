#----------------------------------------------------------------------------
#
# Makefile
#	Postgres documentation installation makefile
#	Thomas Lockhart
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/doc/Makefile,v 1.10 1998/10/06 05:44:05 thomas Exp $
#
#----------------------------------------------------------------------------

PGDOCS= $(POSTGRESDIR)/doc
SRCDIR= ../src

TAR= tar

# Pick up Makefile.global from the source area
# This is the only resource from the code source area and is optional.
# Actually, we want this to get Makefile.custom - thomas 1998-03-01

ifneq ($(wildcard $(SRCDIR)/Makefile.global), )
include $(SRCDIR)/Makefile.global
endif

# Hmm, made this optional but jade _really_ doesn't like them missing
# - thomas 1998-03-01
ifneq ($(HDSL), )
HTMLOPTS= -d $(HDSL)
endif
ifneq ($(PDSL), )
PRINTOPTS= -d $(PDSL)
endif

MODULES= admin postgres programmer tutorial user
TARGETS= $(MODULES:%=%.html)

.PRECIOUS: postgres.tex postgres.dvi
.PHONY: beforeinstall install all clean distclean

beforeinstall::
	if [ ! -d $(PGDOCS) ]; then mkdir $(PGDOCS); fi

install::
	$(MAKE) all
	$(MAKE) man

all:: beforeinstall $(MODULES)

clean::
	rm -rf $(MODULES)

distclean::
	$(MAKE) clean

man:
	$(MAKE) -C $(SRCDIR) install-man

#
# Generic production rules
#

# Unpack tar file
# Put into area pointed to by $(PGDOCS).
## Make a local file to keep track of dependencies,
##  if $(PGDOCS) points somewhere else.
## Disable this for now - thomas 1998-03-01
# Remove the contents of the target directory
#  to replace symlinks - thomas 1998-03-01

%:	%.tar.gz
	rm -rf ./$@ $(PGDOCS)/$*
	if test ! -d $(PGDOCS)/$* ; then mkdir $(PGDOCS)/$* ; fi
	zcat $< | $(TAR) xf - -C $(PGDOCS)/$*
#	touch ./$*

