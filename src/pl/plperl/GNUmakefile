# $Header: /cvsroot/pgsql/src/pl/plperl/GNUmakefile,v 1.5 2000/10/24 17:01:05 tgl Exp $

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

plperl_installdir = $(DESTDIR)$(libdir)


all: Makefile
	$(MAKE) -f $< all

Makefile: Makefile.PL
	@plperl_installdir='$(plperl_installdir)' \
	  EXTRA_INCLUDES='-I$(top_srcdir)/src/include $(INCLUDES)' \
	  $(PERL) $<

install: all installdirs
	$(MAKE) -f Makefile install

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(plperl_installdir)

uninstall:
	rm -f $(DESTDIR)$(libdir)/plperl$(DLSUFFIX)

clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clean
	rm -f Makefile Makefile.old
