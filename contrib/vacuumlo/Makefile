#
# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.8 2000/10/20 21:03:36 petere Exp $
#

subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(libpq_srcdir)
override LIBS += $(libpq)

all: vacuumlo

install: all installdirs
	$(INSTALL_PROGRAM) vacuumlo$(X) $(bindir)
	$(INSTALL_DATA) README.vacuumlo $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/vacuumlo$(X) $(docdir)/contrib/README.vacuumlo

clean distclean maintainer-clean:
	rm -f vacuumlo$(X)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
