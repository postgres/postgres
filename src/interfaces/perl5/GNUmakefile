# This file is an interface from the Autoconf world to Perl's
# MakeMaker world, so that the latter behaves (kind of) like the
# former would prefer. Internally, we call Perl to create another
# Makefile according to its own ideas and then invoke the rules from
# that file.
#
# $Header: /cvsroot/pgsql/src/interfaces/perl5/Attic/GNUmakefile,v 1.6 2001/08/26 22:28:04 petere Exp $

subdir = src/interfaces/perl5
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

.NOTPARALLEL:

# This would allow a non-root install of the Perl module, but it's not
# quite implemented yet.
ifeq ($(mysterious_feature),yes)
perl_installsitearch	= $(pkglibdir)
perl_installsitelib	= $(pkglibdir)
perl_installman3dir	= $(mandir)/man3
endif


all: Makefile libpq-all
	$(MAKE) -f $< all VPATH=$(VPATH)

# We need to run Makefile.PL in the source directory because it scans
# the directory for files to build with.  If we ran it in the build
# dir it would miss all the files and not build half the stuff!
Makefile: Makefile.PL Pg.pm
	abs_builddir=`pwd`; \
	abs_libpq_builddir=`cd $(libpq_builddir) && pwd`; \
	cd $(srcdir) && \
	SRCDIR=$(srcdir) \
	PGLIBDIR=$(libdir) \
	$(PERL) $< \
	  INC='-I$(srcdir) -I$(libpq_srcdir) $(filter -I%, $(CPPFLAGS))' \
	  LIBS="-L$$abs_libpq_builddir -lpq" \
	  INSTALLSITEARCH='$$(DESTDIR)$(perl_installsitearch)' \
	  INSTALLSITELIB='$$(DESTDIR)$(perl_installsitelib)' \
	  INSTALLMAN3DIR='$$(DESTDIR)$(perl_installman3dir)' \
	  MAKEFILE="$$abs_builddir/Makefile"

.PHONY: libpq-all
libpq-all:
	$(MAKE) -C $(libpq_builddir) all


# During install, we must guard against the likelihood that we don't
# have permissions to install into the Perl module library.  It's not
# exactly fun to have to scan the build output, but...

install-warning-msg := { \
echo ""; \
echo "*** Skipping the installation of the Perl module for lack"; \
echo "*** of permissions.  To install it, change to the directory"; \
echo "***     `pwd`,"; \
echo "*** become the appropriate user, and enter '$(MAKE) install'."; \
echo ""; }

# We need to massage the packlist after installation to get the
# DESTDIR out of there.
install: all installdirs
	@if test -w $(DESTDIR)$(perl_installsitearch); then \
	  $(MAKE) -f Makefile pure_install DESTDIR=$(DESTDIR) && \
	  mv $(DESTDIR)$(perl_installsitearch)/auto/Pg/.packlist fake-packlist && \
	  sed 's,^$(DESTDIR),,' fake-packlist >$(DESTDIR)$(perl_installsitearch)/auto/Pg/.packlist && \
	  rm fake-packlist; \
	else \
	  $(install-warning-msg); \
	fi

# Try to make the directories ourselves, otherwise the writability
# test above may fail because of mere non-existence.
installdirs:
	-$(mkinstalldirs) $(DESTDIR)$(perl_installsitearch)/auto/Pg \
	                  $(DESTDIR)$(perl_installsitelib) \
	                  $(DESTDIR)$(perl_installman3dir)

uninstall:
	for file in `cat $(DESTDIR)$(perl_installsitearch)/auto/Pg/.packlist`; do \
	  rm -f $(DESTDIR)$$file || exit; \
	done
	rm -f $(DESTDIR)$(perl_installsitearch)/auto/Pg/.packlist
	-rmdir $(DESTDIR)$(perl_installsitearch)/auto/Pg


clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clean
	rm -f Makefile Makefile.old
	@rm -f fake-packlist


installcheck test: Makefile
	$(MAKE) -f $< test
