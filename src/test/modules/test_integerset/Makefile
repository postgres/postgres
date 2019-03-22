# src/test/modules/test_integerset/Makefile

MODULE_big = test_integerset
OBJS = test_integerset.o $(WIN32RES)
PGFILEDESC = "test_integerset - test code for src/backend/lib/integerset.c"

EXTENSION = test_integerset
DATA = test_integerset--1.0.sql

REGRESS = test_integerset

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_integerset
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
