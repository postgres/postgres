# src/test/modules/test_rls_hooks/Makefile

PGFILEDESC = "test_pg_dump - Test pg_dump with an extension"

EXTENSION = test_pg_dump
DATA = test_pg_dump--1.0.sql

REGRESS = test_pg_dump

check: prove-check

prove-check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_pg_dump
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
