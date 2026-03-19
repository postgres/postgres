# src/test/modules/test_saslprep/Makefile

MODULE_big = test_saslprep
OBJS = \
	$(WIN32RES) \
	test_saslprep.o
PGFILEDESC = "test_saslprep - test SASLprep implementation"

EXTENSION = test_saslprep
DATA = test_saslprep--1.0.sql

REGRESS = test_saslprep

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_saslprep
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
