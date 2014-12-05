# src/test/modules/dummy_seclabel/Makefile

MODULES = dummy_seclabel
PGFILEDESC = "dummy_seclabel - regression testing of the SECURITY LABEL statement"

EXTENSION = dummy_seclabel
DATA = dummy_seclabel--1.0.sql

REGRESS = dummy_seclabel

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/dummy_seclabel
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
