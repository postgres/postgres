#-------------------------------------------------------------------
#
# GNUmakefile for src/interfaces/python, a.k.a. "PyGreSQL"
#
# Written by Peter Eisentraut  <peter_e@gmx.net>
#
# $Header: /cvsroot/pgsql/src/interfaces/python/Attic/GNUmakefile,v 1.1 2000/06/28 18:30:07 petere Exp $
#
#-------------------------------------------------------------------

subdir = src/interfaces/python
top_builddir = ../../..
include ../../Makefile.global

all: Makefile pgmodule.c libpq-all
	$(MAKE) -f Makefile

.PHONY: libpq-all
libpq-all:
	$(MAKE) -C $(top_builddir)/src/interfaces/libpq all

Makefile: Setup.in Makefile.pre.in
	$(MAKE) -f Makefile.pre.in boot srcdir=$(srcdir) VPATH=$(srcdir)

Makefile.pre.in: $(python_extmakefile)
	cp $< $@

Setup.in: Setup.in.raw
	sed -e "s%__LIBPQ__%${top_srcdir}/src/interfaces/libpq%g" \
	    -e "s%__EXTRA_LIBS__%${LIBS}%g" \
	    -e "s%__INCLUDES__%${top_srcdir}/src/include%g" \
	    < $< > $@

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
