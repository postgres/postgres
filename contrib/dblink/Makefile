# $Header: /cvsroot/pgsql/contrib/dblink/Makefile,v 1.4 2001/09/06 10:49:29 petere Exp $

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
