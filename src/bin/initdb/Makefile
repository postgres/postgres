#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/initdb/Makefile,v 1.53 2007/09/27 19:53:43 tgl Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "initdb - initialize a new database cluster"
subdir = src/bin/initdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=	initdb.o $(WIN32RES)

all: submake-libpq submake-libpgport initdb

initdb: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) initdb$(X) '$(DESTDIR)$(bindir)/initdb$(X)'

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/initdb$(X)'

clean distclean maintainer-clean:
	rm -f initdb$(X) $(OBJS)


# ensure that changes in datadir propagate into object file
initdb.o: initdb.c $(top_builddir)/src/Makefile.global
