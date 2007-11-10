# $PostgreSQL: pgsql/contrib/earthdistance/Makefile,v 1.19 2007/11/10 23:59:50 momjian Exp $

MODULES = earthdistance
DATA_built = earthdistance.sql
DATA = uninstall_earthdistance.sql
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
