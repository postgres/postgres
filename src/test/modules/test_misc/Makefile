# src/test/modules/test_misc/Makefile

TAP_TESTS = 1

EXTRA_INSTALL=src/test/modules/injection_points \
	contrib/test_decoding

# The injection points are cluster-wide, so disable installcheck
NO_INSTALLCHECK = 1

export enable_injection_points

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_misc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
