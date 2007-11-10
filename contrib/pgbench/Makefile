# $PostgreSQL: pgsql/contrib/pgbench/Makefile,v 1.16 2007/11/10 23:59:51 momjian Exp $

PROGRAM = pgbench
OBJS	= pgbench.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
