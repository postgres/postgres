# $Header: /cvsroot/pgsql/contrib/dblink/Makefile,v 1.7 2002/09/14 20:28:54 tgl Exp $

subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = dblink
PG_CPPFLAGS = -I$(libpq_srcdir)
OBJS	= dblink.o
SHLIB_LINK = $(libpq)

DATA_built = dblink.sql 
DOCS = README.dblink
REGRESS = dblink

include $(top_srcdir)/contrib/contrib-global.mk
