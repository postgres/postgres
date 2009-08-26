# $PostgreSQL: pgsql/src/makefiles/Makefile,v 1.3 2009/08/26 22:24:43 petere Exp $

subdir = src/makefiles
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


install: all installdirs
	$(INSTALL_DATA) $(srcdir)/pgxs.mk '$(DESTDIR)$(pgxsdir)/$(subdir)/'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/pgxs.mk'
