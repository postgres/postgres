#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.16 2000/06/28 18:29:16 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_dump
top_builddir = ../../..
include ../../Makefile.global

OBJS= pg_dump.o common.o $(STRDUP)

CFLAGS+= -I$(LIBPQDIR)


all: submake pg_dump$(X)

pg_dump$(X): $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBPQ) $(LDFLAGS)

../../utils/strdup.o:
	$(MAKE) -C ../../utils strdup.o

.PHONY: submake
submake:
	$(MAKE) -C $(LIBPQDIR) all

install: all installdirs
	$(INSTALL_PROGRAM) pg_dump$(X) $(bindir)/pg_dump$(X)
	$(INSTALL_SCRIPT) pg_dumpall $(bindir)/pg_dumpall
	$(INSTALL_SCRIPT) pg_upgrade $(bindir)/pg_upgrade

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(addprefix $(bindir)/, pg_dump$(X) pg_dumpall pg_upgrade)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_dump$(X) $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
