# src/test/modules/test_ginpostinglist/Makefile

MODULE_big = test_ginpostinglist
OBJS = \
	$(WIN32RES) \
	test_ginpostinglist.o
PGFILEDESC = "test_ginpostinglist - test code for src/backend/access/gin//ginpostinglist.c"

EXTENSION = test_ginpostinglist
DATA = test_ginpostinglist--1.0.sql

REGRESS = test_ginpostinglist

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_ginpostinglist
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
