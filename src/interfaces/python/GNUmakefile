#-------------------------------------------------------------------
#
# GNUmakefile for src/interfaces/python, a.k.a. "PyGreSQL"
#
# Written by Peter Eisentraut  <peter_e@gmx.net>
#
# $Header: /cvsroot/pgsql/src/interfaces/python/Attic/GNUmakefile,v 1.2 2000/08/31 16:12:01 petere Exp $
#
#-------------------------------------------------------------------

subdir = src/interfaces/python
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: Makefile pgmodule.c libpq-all
	$(MAKE) -f Makefile

.PHONY: libpq-all
libpq-all:
	$(MAKE) -C $(libpq_builddir) all

Makefile: Setup.in Makefile.pre.in
	$(MAKE) -f Makefile.pre.in boot srcdir=$(srcdir) VPATH=$(srcdir)

Makefile.pre.in: $(python_extmakefile)
	cp $< $@

Setup.in: Setup.in.raw
	sed -e 's,@libpq_srcdir@,$(libpq_srcdir),g' \
	    -e 's,@libpq_builddir@,$(libpq_builddir),g' \
	    -e 's,@EXTRA_LIBS@,$(LIBS),g' \
	    -e 's,@pg_includedir@,$(top_srcdir)/src/include,g' \
	    $< > $@

install: all
	@echo "Installing Python module"
	@if ! ( $(INSTALL_DATA) pg.py $(python_moduledir) && \
	    $(MAKE) -f Makefile install ); then \
		echo "*****" ;\
		echo "* Skipping the installation of the Python interface module for lack"; \
		echo "* of permissions. To install it, change to the directory"; \
		echo "* "`pwd`", become the appropriate"; \
		echo "* user, and do \`$(MAKE) install'."; \
		echo "*****"; \
	fi

uninstall:
	@echo "*****"; \
	 echo "* Unfortunately, the Python interface module cannot be uninstalled"; \
	 echo "* automatically. To do it yourself, look in or near the directory"; \
	 echo "* \`$(python_moduledir)' for files \`pg.py' and \`_pgmodule$(DLSUFFIX)'."; \
	 echo "*****"

# Python sometimes has a different idea what exactly "clean" is.

clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clobber
	rm -f Makefile.pre.in Makefile Setup Setup.in
