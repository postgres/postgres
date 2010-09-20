#-------------------------------------------------------------------------
#
# Makefile for src/tools/fsync
#
# Copyright (c) 2003-2010, PostgreSQL Global Development Group
#
# src/tools/fsync/Makefile
#
#-------------------------------------------------------------------------

subdir = src/tools/fsync
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS= test_fsync.o

all: submake-libpq submake-libpgport test_fsync

test_fsync: test_fsync.o $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) test_fsync.o $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

clean distclean maintainer-clean:
	rm -f test_fsync$(X) $(OBJS)
