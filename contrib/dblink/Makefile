# $Header: /cvsroot/pgsql/contrib/dblink/Makefile,v 1.5 2002/06/23 21:58:07 momjian Exp $

subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = dblink
PG_CPPFLAGS = -I$(libpq_srcdir)
OBJS	= dblink.o
SHLIB_LINK = $(libpq)

DATA_built = dblink.sql 
DOCS = README.dblink

include $(top_srcdir)/contrib/contrib-global.mk
