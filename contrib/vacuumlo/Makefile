# $Header: /cvsroot/pgsql/contrib/vacuumlo/Makefile,v 1.11 2001/09/06 10:49:30 petere Exp $

subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = vacuumlo
OBJS	= vacuumlo.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.vacuumlo

include $(top_srcdir)/contrib/contrib-global.mk
