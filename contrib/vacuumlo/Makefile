#
# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.6 2000/07/09 13:13:55 petere Exp $
#

subdir = contrib/vacuumlo
top_builddir = ../..
include ../../src/Makefile.global

CPPFLAGS += -I$(LIBPQDIR)
LIBS += $(LIBPQ)

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
