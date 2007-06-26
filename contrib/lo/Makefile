# $PostgreSQL: pgsql/contrib/lo/Makefile,v 1.16 2007/06/26 22:05:02 tgl Exp $

MODULES = lo
DATA_built = lo.sql
DATA = uninstall_lo.sql
DOCS = README.lo

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
