#-------------------------------------------------------------------------
#
# Makefile for utils
#
# $PostgreSQL: pgsql/src/utils/Makefile,v 1.21 2004/07/30 12:26:40 petere Exp $
#
# dllinit.o is only built on Win32 platform.
#
#-------------------------------------------------------------------------

subdir = src/utils
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


ifdef DLLINIT

all:

install: all installdirs
	$(INSTALL_DATA) dllinit.o $(DESTDIR)$(pgxsdir)/$(subdir)/

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(pgxsdir)/$(subdir)

uninstall:
	rm -f $(DESTDIR)$(pgxsdir)/$(subdir)/dllinit.o

clean distclean maintainer-clean:
	rm -f dllinit.o

endif # not DLLINIT
