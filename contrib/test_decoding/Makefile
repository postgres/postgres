# contrib/test_decoding/Makefile

MODULES = test_decoding
PGFILEDESC = "test_decoding - example of a logical decoding output plugin"

REGRESS = ddl xact rewrite toast permissions decoding_in_xact \
	decoding_into_rel binary prepared replorigin time messages \
	spill slot truncate
ISOLATION = mxact delayed_startup ondisk_startup concurrent_ddl_dml \
	oldest_xmin snapshot_transfer subxact_without_top catalog_change_snapshot

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/test_decoding/logical.conf
ISOLATION_OPTS = --temp-config $(top_srcdir)/contrib/test_decoding/logical.conf

# Disabled because these tests require "wal_level=logical", which
# typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_decoding
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# But it can nonetheless be very helpful to run tests on preexisting
# installation, allow to do so, but only if requested explicitly.
installcheck-force:
	$(pg_regress_installcheck) $(REGRESS)
	$(pg_isolation_regress_installcheck) $(ISOLATION)
