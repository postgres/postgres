# src/test/modules/test_regex/Makefile

MODULE_big = test_regex
OBJS = \
	$(WIN32RES) \
	test_regex.o
PGFILEDESC = "test_regex - test code for backend/regex/"

EXTENSION = test_regex
DATA = test_regex--1.0.sql

REGRESS = test_regex test_regex_utf8

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_regex
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
