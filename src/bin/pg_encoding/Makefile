#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_encoding
#
# Copyright (c) 1998, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/src/bin/pg_encoding/Attic/Makefile,v 1.12 2000/11/30 20:36:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_encoding
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= pg_encoding.o

all: submake pg_encoding

pg_encoding: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(libpq) $(LIBS) -o $@

.PHONY: submake

submake:
	$(MAKE) -C $(libpq_builddir) all

install: all installdirs
	$(INSTALL_PROGRAM) pg_encoding$(X) $(DESTDIR)$(bindir)/pg_encoding$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_encoding$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_encoding$(X) pg_encoding.o

ifeq (depend,$(wildcard depend))
include depend
endif
