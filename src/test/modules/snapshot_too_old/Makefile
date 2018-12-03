# src/test/modules/snapshot_too_old/Makefile

# Note: because we don't tell the Makefile there are any regression tests,
# we have to clean those result files explicitly
EXTRA_CLEAN = $(pg_regress_clean_files)

ISOLATION = sto_using_cursor sto_using_select sto_using_hash_index
ISOLATION_OPTS = --temp-config $(top_srcdir)/src/test/modules/snapshot_too_old/sto.conf

# Disabled because these tests require "old_snapshot_threshold" >= 0, which
# typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/snapshot_too_old
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# But it can nonetheless be very helpful to run tests on preexisting
# installation, allow to do so, but only if requested explicitly.
installcheck-force:
	$(pg_isolation_regress_installcheck) $(ISOLATION)
