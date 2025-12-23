# src/test/modules/test_lwlock_tranches/Makefile

MODULE_big = test_lwlock_tranches
OBJS = \
	$(WIN32RES) \
	test_lwlock_tranches.o
PGFILEDESC = "test_lwlock_tranches - test code for LWLock tranches allocated by extensions"

EXTENSION = test_lwlock_tranches
DATA = test_lwlock_tranches--1.0.sql

REGRESS_OPTS = --temp-config $(top_srcdir)/src/test/modules/test_lwlock_tranches/test_lwlock_tranches.conf
REGRESS = test_lwlock_tranches
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_lwlock_tranches
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
