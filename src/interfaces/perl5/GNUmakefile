# This file is an interface from the Autoconf world to Perl's
# MakeMaker world, so that the latter behaves (kind of) like the
# former would prefer. Internally, we call Perl to create another
# Makefile according to its own ideas and then invoke the rules from
# that file.
#
# $Header: /cvsroot/pgsql/src/interfaces/perl5/Attic/GNUmakefile,v 1.3 2000/10/24 16:59:59 tgl Exp $

subdir = src/interfaces/perl5
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


all: Makefile libpq-all
	$(MAKE) -f $< all

Makefile: Makefile.PL
	$(PERL) $<

.PHONY: libpq-all
libpq-all:
	$(MAKE) -C $(libpq_builddir) all

# The klugery here is to ensure that the perl5 shared library gets
# built with the correct path to the installed location of libpq
# during `make install', but is built against the local tree during
# ordinary building and testing.
#
# During install, we must also guard against the likelihood that we
# don't have permissions to install into the Perl module library. The
# purer alternative would naturally be the ability to select the
# installation directory somewhere.

install: Makefile
	$(MAKE) -f Makefile clean
	POSTGRES_LIB="$(libdir)" \
	  POSTGRES_INCLUDE="$(includedir)" \
	  $(PERL) $(srcdir)/Makefile.PL
	$(MAKE) -f Makefile all
	-@if [ -w "`$(MAKE) --quiet -f Makefile echo-installdir`" ]; then \
		$(MAKE) -f Makefile install; \
		rm -f Makefile; \
	else \
		echo "*****" ;\
		echo "* Skipping the installation of the Perl module for lack of permissions."; \
		echo "* To install it, change to the directory "`pwd`","; \
		echo "* become the appropriate user, and do \`$(MAKE) install'."; \
		echo "*****"; \
	fi


uninstall:
	@echo "*****"; \
	 echo "* The Perl module cannot be uninstalled automatically. You can"; \
	 echo "* change into the directory "`pwd`" and do"; \
	 echo "*"; \
	 echo "*     $(MAKE) -f Makefile realclean"; \
	 echo "*"; \
	 echo "* to delete built and installed files."; \
	 echo "*****"

clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clean
	rm -f Makefile Makefile.old
