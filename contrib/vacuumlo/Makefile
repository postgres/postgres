# $PostgreSQL: pgsql/contrib/vacuumlo/Makefile,v 1.12 2003/11/29 19:51:36 pgsql Exp $

subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = vacuumlo
OBJS	= vacuumlo.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.vacuumlo

include $(top_srcdir)/contrib/contrib-global.mk
