#-------------------------------------------------------------------------
#
# Makefile for tools/thread
#
# Copyright (C) 2003 by PostgreSQL Global Development Team
#
# $PostgreSQL: pgsql/src/tools/thread/Makefile,v 1.4 2004/04/23 18:15:55 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = tools/thread
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(THREAD_SUPPORT), no)
$(error Your platform does not support threads)
endif
ifeq ($(THREAD_SUPPORT), )
$(error You have not configured your template/$$port file.  See the README)
endif

override CFLAGS += $(PTHREAD_CFLAGS)

LDFLAGS += $(PTHREAD_LIBS)

all: thread_test

thread_test: thread_test.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

clean distclean maintainer-clean:
	rm -f thread_test$(X) thread_test.o
