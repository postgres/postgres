# src/test/modules/test_rls_hooks/Makefile

MODULE_big = test_rls_hooks
OBJS = test_rls_hooks.o $(WIN32RES)
PGFILEDESC = "test_rls_hooks - example use of RLS hooks"

EXTENSION = test_rls_hooks
# DATA = test_rls_hooks--1.0.sql

REGRESS = test_rls_hooks
REGRESS_OPTS = --temp-config=$(top_srcdir)/src/test/modules/test_rls_hooks/rls_hooks.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_rls_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
