# src/test/modules/plsample/Makefile

MODULES = plsample

EXTENSION = plsample
DATA = plsample--1.0.sql
PGFILEDESC = "PL/Sample - template for procedural language"

REGRESS = plsample

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/plsample
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
