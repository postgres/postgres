#-------------------------------------------------------------------------
#
# Makefile for tools/thread
#
# Copyright (C) 2003 by PostgreSQL Global Development Team
#
# $PostgreSQL: pgsql/src/tools/thread/Makefile,v 1.3 2003/11/29 19:52:14 pgsql Exp $
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
