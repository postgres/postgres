# $PostgreSQL: pgsql/contrib/pgbench/Makefile,v 1.11 2003/11/29 19:51:35 pgsql Exp $

subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = pgbench
OBJS	= pgbench.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.pgbench README.pgbench_jis

include $(top_srcdir)/contrib/contrib-global.mk
