# src/test/modules/commit_ts/Makefile

REGRESS = commit_timestamp
REGRESS_OPTS = --temp-config=$(top_srcdir)/src/test/modules/commit_ts/commit_ts.conf
# Disabled because these tests require "track_commit_timestamp = on",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/commit_ts
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
