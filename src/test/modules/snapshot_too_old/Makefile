# src/test/modules/snapshot_too_old/Makefile

EXTRA_CLEAN = ./isolation_output

ISOLATIONCHECKS=sto_using_cursor sto_using_select

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

# Disabled because these tests require "old_snapshot_threshold" >= 0, which
# typical installcheck users do not have (e.g. buildfarm clients).
installcheck:;

# But it can nonetheless be very helpful to run tests on preexisting
# installation, allow to do so, but only if requested explicitly.
installcheck-force: isolationcheck-install-force

check: isolationcheck

submake-isolation:
	$(MAKE) -C $(top_builddir)/src/test/isolation all

submake-test_snapshot_too_old:
	$(MAKE) -C $(top_builddir)/src/test/modules/snapshot_too_old

isolationcheck: | submake-isolation submake-test_snapshot_too_old temp-install
	$(MKDIR_P) isolation_output
	$(pg_isolation_regress_check) \
	    --temp-config $(top_srcdir)/src/test/modules/snapshot_too_old/sto.conf \
	    --outputdir=./isolation_output \
	    $(ISOLATIONCHECKS)

isolationcheck-install-force: all | submake-isolation submake-test_snapshot_too_old temp-install
	$(pg_isolation_regress_installcheck) \
	    $(ISOLATIONCHECKS)

.PHONY: check submake-test_snapshot_too_old isolationcheck isolationcheck-install-force

temp-install: EXTRA_INSTALL=src/test/modules/snapshot_too_old
