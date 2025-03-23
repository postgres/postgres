# src/test/modules/csnsnapshot/Makefile

NO_INSTALLCHECK = 1

TAP_TESTS = 1

# Doesn't support full consistency of distributed commit in READ COMMITTED
# transactions.
PROVE_TESTS =	t/001_base.pl \
				t/002_standby.pl \
				t/003_parallel_safe.pl

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/csnsnapshot
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
