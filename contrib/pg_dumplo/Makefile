#
# $Header: /cvsroot/pgsql/contrib/pg_dumplo/Attic/Makefile,v 1.10 2001/02/20 19:20:27 petere Exp $
#

subdir = contrib/pg_dumplo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= main.o lo_export.o lo_import.o utils.o

all: pg_dumplo

pg_dumplo: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pg_dumplo$(X)	$(bindir)
	$(INSTALL_DATA) README.pg_dumplo	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pg_dumplo$(X) $(docdir)/contrib/README.pg_dumplo

clean distclean maintainer-clean:
	rm -f pg_dumplo$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
