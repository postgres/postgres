# src/test/modules/test_session_hooks/Makefile

MODULES = test_session_hooks
PGFILEDESC = "test_session_hooks - Test session hooks with an extension"

EXTENSION = test_session_hooks
DATA = test_session_hooks--1.0.sql

REGRESS = test_session_hooks
REGRESS_OPTS = --temp-config=$(top_srcdir)/src/test/modules/test_session_hooks/session_hooks.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_session_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# override installcheck - this module requires preloading the test module
installcheck:
	@echo Cannot run $@ for test_session_hooks. Run "'make check'" instead.
