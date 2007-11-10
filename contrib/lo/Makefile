# $PostgreSQL: pgsql/contrib/lo/Makefile,v 1.17 2007/11/10 23:59:51 momjian Exp $

MODULES = lo
DATA_built = lo.sql
DATA = uninstall_lo.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/lo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
