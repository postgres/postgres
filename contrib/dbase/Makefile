# $Header: /cvsroot/pgsql/contrib/dbase/Attic/Makefile,v 1.2 2001/09/06 10:49:29 petere Exp $

subdir = contrib/dbase
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = dbf2pg
OBJS	= dbf.o dbf2pg.o endian.o
PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.dbf2pg
MAN = dbf2pg.1			# XXX not implemented

include $(top_srcdir)/contrib/contrib-global.mk
