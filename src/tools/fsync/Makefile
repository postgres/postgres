#-------------------------------------------------------------------------
#
# Makefile for src/tools/fsync
#
# Copyright (c) 2003-2009, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/tools/fsync/Makefile,v 1.7 2009/01/01 17:24:05 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/tools/fsync
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS= test_fsync.o

all: submake-libpq submake-libpgport test_fsync

test_fsync: test_fsync.o $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) test_fsync.o $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

clean distclean maintainer-clean:
	rm -f test_fsync$(X) $(OBJS)
