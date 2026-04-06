# src/test/modules/test_autovacuum/Makefile

PGFILEDESC = "test_autovacuum - test code for autovacuum"

TAP_TESTS = 1

EXTRA_INSTALL = src/test/modules/injection_points

export enable_injection_points

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_autovacuum
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
