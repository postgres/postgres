#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_resetxlog
#
# Copyright (c) 1998-2002, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_resetxlog/Makefile,v 1.7 2004/04/26 17:40:48 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_resetxlog
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := $(CPPFLAGS) -DFRONTEND

OBJS= pg_resetxlog.o pg_crc.o \
      $(filter dirmod.o, $(LIBOBJS))

all: submake-libpgport pg_resetxlog

pg_resetxlog: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@$(X)

dirmod.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_resetxlog$(X) $(DESTDIR)$(bindir)/pg_resetxlog$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_resetxlog$(X)

clean distclean maintainer-clean:
	rm -f pg_resetxlog$(X) pg_crc.c dirmod.c $(OBJS)
