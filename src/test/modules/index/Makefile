# src/test/modules/index/Makefile

EXTRA_INSTALL = contrib/btree_gin contrib/btree_gist

ISOLATION = killtuples

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/index
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
