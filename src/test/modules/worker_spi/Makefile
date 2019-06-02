# src/test/modules/worker_spi/Makefile

MODULES = worker_spi

EXTENSION = worker_spi
DATA = worker_spi--1.0.sql
PGFILEDESC = "worker_spi - background worker example"

REGRESS = worker_spi

# enable our module in shared_preload_libraries for dynamic bgworkers
REGRESS_OPTS = --temp-config $(top_srcdir)/src/test/modules/worker_spi/dynamic.conf

# Disable installcheck to ensure we cover dynamic bgworkers.
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/worker_spi
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
