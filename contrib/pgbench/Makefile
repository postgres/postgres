# $Header: /cvsroot/pgsql/contrib/pgbench/Makefile,v 1.10 2001/09/06 10:49:30 petere Exp $

subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = pgbench
OBJS	= pgbench.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.pgbench README.pgbench_jis

include $(top_srcdir)/contrib/contrib-global.mk
