#-------------------------------------------------------------------------
#
# Makefile for src/tools/findoidjoins
#
# Copyright (c) 2003-2009, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/tools/findoidjoins/Makefile,v 1.6 2009/01/01 17:24:05 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/tools/findoidjoins
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS= findoidjoins.o

all: submake-libpq submake-libpgport findoidjoins

findoidjoins: findoidjoins.o $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) findoidjoins.o $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

clean distclean maintainer-clean:
	rm -f findoidjoins$(X) $(OBJS)
