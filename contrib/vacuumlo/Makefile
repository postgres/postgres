#
# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.7 2000/08/31 16:09:12 petere Exp $
#

subdir = contrib/vacuumlo
top_builddir = ../..
include ../../src/Makefile.global

CPPFLAGS += -I$(libpq_srcdir)
LIBS += $(libpq)

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
