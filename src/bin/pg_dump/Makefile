#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_dump
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/pg_dump/Makefile,v 1.17 2000/07/03 16:35:39 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_dump
top_builddir = ../../..
include ../../Makefile.global

OBJS= pg_dump.o common.o $(STRDUP)

CFLAGS+= -I$(LIBPQDIR)


all: submake pg_dump pg_dumpall

pg_dump: $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBPQ) $(LDFLAGS)

pg_dumpall: pg_dumpall.sh
	sed -e 's:__VERSION__:$(VERSION):g' \
	    -e 's:__MULTIBYTE__:$(MULTIBYTE):g' \
	    -e 's:__bindir__:$(bindir):g' \
	  < $< > $@

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
	rm -f pg_dump$(X) $(OBJS) pg_dumpall

ifeq (depend,$(wildcard depend))
include depend
endif
