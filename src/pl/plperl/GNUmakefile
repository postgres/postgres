# $Header: /cvsroot/pgsql/src/pl/plperl/GNUmakefile,v 1.6 2000/11/02 18:40:13 petere Exp $

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

plperl_installdir = $(DESTDIR)$(libdir)


all: Makefile
	$(MAKE) -f $< all

Makefile: Makefile.PL
	plperl_installdir='$(plperl_installdir)' \
	EXTRA_INCLUDES='$(filter -I%, $(CPPFLAGS))' \
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
