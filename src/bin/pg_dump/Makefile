#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.19 2000/07/04 19:52:00 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_dump
top_builddir = ../../..
include ../../Makefile.global

OBJS= pg_backup_archiver.o pg_backup_custom.o pg_backup_files.o \
       pg_backup_plain_text.o $(STRDUP)

CFLAGS+= -I$(LIBPQDIR)
LIBS+= -lz

all: submake pg_dump pg_restore pg_dumpall

pg_dump: pg_dump.o common.o $(OBJS) $(LIBPQDIR)/libpq.a 
	$(CC) $(CFLAGS) -o $@ pg_dump.o common.o $(OBJS) $(LIBPQ) $(LDFLAGS)

pg_restore: pg_restore.o $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(CFLAGS) -o $@ pg_restore.o $(OBJS) $(LIBPQ) $(LDFLAGS)

../../utils/strdup.o:
	$(MAKE) -C ../../utils strdup.o

pg_dumpall: pg_dumpall.sh
	sed -e 's:__VERSION__:$(VERSION):g' \
	    -e 's:__MULTIBYTE__:$(MULTIBYTE):g' \
	    -e 's:__bindir__:$(bindir):g' \
	  < $< > $@

.PHONY: submake
submake:
	$(MAKE) -C $(LIBPQDIR) all

install: all installdirs
	$(INSTALL_PROGRAM) pg_dump$(X) $(bindir)/pg_dump$(X)
	$(INSTALL_PROGRAM) pg_restore$(X) $(bindir)/pg_restore$(X)
	$(INSTALL_SCRIPT) pg_dumpall $(bindir)/pg_dumpall
	$(INSTALL_SCRIPT) pg_upgrade $(bindir)/pg_upgrade

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(addprefix $(bindir)/, pg_dump$(X) pg_restore$(X) pg_dumpall pg_upgrade)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_dump$(X) pg_restore$(X) $(OBJS) pg_dump.o common.o pg_restore.o pg_dumpall

ifeq (depend,$(wildcard depend))
include depend
endif
