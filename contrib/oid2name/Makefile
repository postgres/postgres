# $Header: /cvsroot/pgsql/contrib/oid2name/Makefile,v 1.4 2001/09/06 10:49:29 petere Exp $

subdir = contrib/oid2name
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = oid2name
OBJS	= oid2name.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.oid2name

include $(top_srcdir)/contrib/contrib-global.mk
