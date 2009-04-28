# $PostgreSQL: pgsql/contrib/pg_trgm/Makefile,v 1.10 2009/04/28 17:07:50 momjian Exp $

MODULE_big = pg_trgm
OBJS = trgm_op.o trgm_gist.o trgm_gin.o

DATA_built = pg_trgm.sql
DATA = uninstall_pg_trgm.sql
REGRESS = pg_trgm

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_trgm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
