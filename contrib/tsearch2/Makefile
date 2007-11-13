# $PostgreSQL: pgsql/contrib/tsearch2/Makefile,v 1.20 2007/11/13 21:02:28 tgl Exp $

MODULES = tsearch2
DATA_built = tsearch2.sql
DATA = uninstall_tsearch2.sql
REGRESS = tsearch2

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
