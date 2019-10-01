# src/test/modules/test_session_hooks/Makefile

MODULE_big = test_session_hooks
OBJS = test_session_hooks.o $(WIN32RES)
PGFILEDESC = "test_session_hooks - tests for start and end session hooks"

REGRESS = test_session_hooks
REGRESS_OPTS = --temp-config=$(top_srcdir)/src/test/modules/test_session_hooks/session_hooks.conf
# Disabled because these tests require extra configuration with
# "shared_preload_libraries=test_session_hooks", which typical
# installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

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
