#
# $Header: /cvsroot/pgsql/contrib/pg_dumplo/Attic/Makefile,v 1.5 2000/07/09 13:13:24 petere Exp $
#

subdir = contrib/pg_dumplo
top_builddir = ../..
include ../../src/Makefile.global

OBJS	= main.o lo_export.o lo_import.o utils.o
CFLAGS += -I$(LIBPQDIR)

all: pg_dumplo

pg_dumplo: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBPQ) 

install: all installdirs
	$(INSTALL_PROGRAM) pg_dumplo$(X)	$(bindir)
	$(INSTALL_DATA) README.pg_dumplo	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pg_dumplo$(X) $(docdir)/contrib/README.pg_dumplo

clean distclean maintainer-clean:
	rm -f pg_dumplo $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
