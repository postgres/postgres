# $PostgreSQL: pgsql/contrib/earthdistance/Makefile,v 1.18 2007/06/26 22:05:02 tgl Exp $

MODULES = earthdistance
DATA_built = earthdistance.sql
DATA = uninstall_earthdistance.sql
DOCS = README.earthdistance
REGRESS = earthdistance

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/earthdistance
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
