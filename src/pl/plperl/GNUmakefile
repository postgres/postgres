# $Header: /cvsroot/pgsql/src/pl/plperl/GNUmakefile,v 1.2 2000/06/28 18:30:12 petere Exp $

subdir = src/pl/plperl
top_builddir = ../../..
include ../../Makefile.global

plperl_installdir = $(libdir)


all: Makefile
	$(MAKE) -f $< all

Makefile: Makefile.PL
	@plperl_installdir='$(plperl_installdir)' \
	  EXTRA_INCLUDES='-I$(top_srcdir)/src/include $(INCLUDES)' \
	  $(PERL) $< POLLUTE=1

install: all installdirs
	$(MAKE) -f $< install

installdirs:
	$(mkinstalldirs) $(plperl_installdir)

uninstall:
	rm -f $(libdir)/plperl$(DLSUFFIX)

clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clean
	rm -f Makefile Makefile.old
