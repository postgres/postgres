#-------------------------------------------------------------------------
#
# Makefile for src/test/modules/test_checksums
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/modules/test_checksums/Makefile
#
#-------------------------------------------------------------------------

EXTRA_INSTALL = src/test/modules/injection_points

export enable_injection_points

MODULE_big = test_checksums
OBJS = \
	$(WIN32RES) \
	test_checksums.o
PGFILEDESC = "test_checksums - test code for data checksums"

EXTENSION = test_checksums
DATA = test_checksums--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_checksums
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
