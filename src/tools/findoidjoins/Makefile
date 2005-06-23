#-------------------------------------------------------------------------
#
# Makefile for src/tools/findoidjoins
#
# Copyright (c) 2003-2005, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/tools/findoidjoins/Makefile,v 1.1 2005/06/23 02:33:28 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/tools/findoidjoins
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) $(CPPFLAGS)

OBJS= findoidjoins.o

all: submake-libpq submake-libpgport findoidjoins

findoidjoins: findoidjoins.o $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) findoidjoins.o $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

clean distclean maintainer-clean:
	rm -f findoidjoins$(X) $(OBJS)
