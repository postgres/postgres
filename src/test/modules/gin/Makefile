# src/test/modules/gin/Makefile

EXTRA_INSTALL = src/test/modules/injection_points

REGRESS = gin_incomplete_splits

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/gin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
