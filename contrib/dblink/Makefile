# $Header: /cvsroot/pgsql/contrib/dblink/Makefile,v 1.6 2002/09/03 04:01:05 tgl Exp $

subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = dblink
PG_CPPFLAGS = -I$(libpq_srcdir)
OBJS	= dblink.o
SHLIB_LINK = $(libpq)

DATA_built = dblink.sql 
DOCS = README.dblink

EXTRA_CLEAN = dblink.test.out dblink.test.diff

include $(top_srcdir)/contrib/contrib-global.mk
