#-------------------------------------------------------------------------
#
# Makefile for tools/thread
#
# Copyright (C) 2003 by PostgreSQL Global Development Team
#
# $PostgreSQL: pgsql/src/tools/thread/Makefile,v 1.6 2004/04/25 20:47:56 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = tools/thread
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CFLAGS += $(PTHREAD_CFLAGS)

all: thread_test

thread_test: thread_test.o
# no need for $LIBS, might not be compiled yet
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(PTHREAD_LIBS) -o $@

clean distclean maintainer-clean:
	rm -f thread_test$(X) thread_test.o
