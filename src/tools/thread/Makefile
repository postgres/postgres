#-------------------------------------------------------------------------
#
# Makefile for tools/thread
#
# Copyright (c) 2003-2005, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/tools/thread/Makefile,v 1.8 2005/01/01 20:44:34 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/tools/thread
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CFLAGS += $(PTHREAD_CFLAGS)

all: thread_test

thread_test: thread_test.o
# no need for $LIBS, might not be compiled yet
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(PTHREAD_LIBS) -o $@

clean distclean maintainer-clean:
	rm -f thread_test$(X) thread_test.o
