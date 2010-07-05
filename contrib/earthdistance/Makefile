# $PostgreSQL: pgsql/contrib/earthdistance/Makefile,v 1.20 2010/07/05 23:15:55 tgl Exp $

MODULES = earthdistance
DATA_built = earthdistance.sql
DATA = uninstall_earthdistance.sql
REGRESS = earthdistance

LDFLAGS_SL += $(filter -lm, $(LIBS))

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
