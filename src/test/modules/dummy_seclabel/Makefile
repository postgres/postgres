# src/test/modules/dummy_seclabel/Makefile

MODULES = dummy_seclabel
PGFILEDESC = "dummy_seclabel - regression testing of the SECURITY LABEL statement"

REGRESS = dummy_seclabel
EXTRA_CLEAN = sql/dummy_seclabel.sql expected/dummy_seclabel.out

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/dummy_seclabel
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
