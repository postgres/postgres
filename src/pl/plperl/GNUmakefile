# $Header: /cvsroot/pgsql/src/pl/plperl/GNUmakefile,v 1.7 2001/08/26 23:54:41 petere Exp $

subdir = src/pl/plperl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(allow_nonpic_in_shlib),yes)
makefile_pl_flags = --force
endif


all: Makefile
	$(MAKE) -f $< all VPATH=$(VPATH)

Makefile: Makefile.PL
	plperl_installdir='$$(DESTDIR)$(libdir)' \
	$(PERL) $< $(makefile_pl_flags) INC='-I$(srcdir) $(filter -I%, $(CPPFLAGS))'

install: all installdirs
	$(MAKE) -f Makefile install DESTDIR='$(DESTDIR)'

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(libdir)

uninstall:
	rm -f $(DESTDIR)$(libdir)/plperl$(DLSUFFIX)

clean distclean maintainer-clean:
	-[ -f Makefile ] && $(MAKE) -f Makefile clean
	rm -f Makefile Makefile.old
