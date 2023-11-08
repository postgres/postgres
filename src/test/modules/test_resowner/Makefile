# src/test/modules/test_resowner/Makefile

MODULE_big = test_resowner
OBJS = \
	$(WIN32RES) \
	test_resowner_basic.o \
	test_resowner_many.o
PGFILEDESC = "test_resowner - test code for ResourceOwners"

EXTENSION = test_resowner
DATA = test_resowner--1.0.sql

REGRESS = test_resowner

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_resowner
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
