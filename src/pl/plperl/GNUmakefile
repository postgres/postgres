# $Header: /cvsroot/pgsql/src/pl/plperl/GNUmakefile,v 1.9 2002/02/23 21:49:10 momjian Exp $

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(allow_nonpic_in_shlib),yes)
makefile_pl_flags = --force
endif


all: Makefile
	$(MAKE) -f $< all VPATH=$(VPATH)

Makefile: Makefile.PL
	plperl_installdir='$$(DESTDIR)$(pkglibdir)' \
	$(PERL) $< $(makefile_pl_flags) LIBS='$(BE_DLLLIBS)' \
	INC='-I$(srcdir) $(filter -I%, $(CPPFLAGS))'

install: all installdirs
	$(MAKE) -f Makefile install DESTDIR='$(DESTDIR)'

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(pkglibdir)

uninstall:
	rm -f $(DESTDIR)$(pkglibdir)/plperl$(DLSUFFIX)

clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clean
	rm -f Makefile Makefile.old
