#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_version
#
# Portions Copyright (c) 1996-2000, PostgreSQL, Inc
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pg_version/Attic/Makefile,v 1.7 2000/06/27 00:31:29 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_version
top_builddir = ../../..
include ../../Makefile.global

OBJS= pg_version.o $(top_builddir)/src/utils/version.o $(STRERROR2)

all: pg_version$(X)

pg_version$(X): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(top_builddir)/src/utils/version.o: $(top_srcdir)/src/utils/version.c $(top_builddir)/src/include/version.h
	$(MAKE) -C $(top_builddir)/src/utils version.o

install: all installdirs
	$(INSTALL_PROGRAM) pg_version$(X) $(bindir)/pg_version$(X)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/pg_version$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_version$(X) pg_version.o

ifeq (depend,$(wildcard depend))
include depend
endif
