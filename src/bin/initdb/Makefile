#-------------------------------------------------------------------------
#
# Makefile for src/bin/initdb
#
# Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/initdb/Makefile,v 1.33 2003/11/23 21:42:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/initdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DPGBINDIR=\"$(bindir)\" -DPGDATADIR=\"$(datadir)\" -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=	initdb.o

all: submake-libpq submake-libpgport initdb

initdb: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) initdb$(X) $(DESTDIR)$(bindir)/initdb$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/initdb$(X)

clean distclean maintainer-clean:
	rm -f initdb$(X) $(OBJS)
