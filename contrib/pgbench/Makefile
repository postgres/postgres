# $PostgreSQL: pgsql/contrib/pgbench/Makefile,v 1.12.4.1 2005/03/25 18:18:39 momjian Exp $

PROGRAM = pgbench
OBJS	= pgbench.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

DOCS = README.pgbench README.pgbench_jis

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
