# $Header: /cvsroot/pgsql/contrib/findoidjoins/Attic/Makefile,v 1.14 2002/09/05 19:57:32 tgl Exp $

subdir = contrib/findoidjoins
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = findoidjoins
OBJS	= findoidjoins.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

SCRIPTS = make_oidjoins_check
DOCS = README.findoidjoins

include $(top_srcdir)/contrib/contrib-global.mk
