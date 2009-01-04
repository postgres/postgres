# $PostgreSQL: pgsql/contrib/pg_stat_statements/Makefile,v 1.1 2009/01/04 22:19:59 tgl Exp $

MODULE_big = pg_stat_statements
DATA_built = pg_stat_statements.sql
DATA = uninstall_pg_stat_statements.sql
OBJS = pg_stat_statements.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_statements
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
