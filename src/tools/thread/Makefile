#-------------------------------------------------------------------------
#
# Makefile for tools/thread
#
# Copyright (C) 2003 by PostgreSQL Global Development Team
#
# $Header: /cvsroot/pgsql/src/tools/thread/Attic/Makefile,v 1.2 2003/09/27 16:24:45 momjian Exp $
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

override CFLAGS += $(THREAD_CPPFLAGS)

LDFLAGS += $(THREAD_LIBS)

all: thread_test

thread_test: thread_test.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

clean distclean maintainer-clean:
	rm -f thread_test$(X) thread_test.o
