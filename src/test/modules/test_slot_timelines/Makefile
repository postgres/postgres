# src/test/modules/test_slot_timelines/Makefile

MODULES = test_slot_timelines
PGFILEDESC = "test_slot_timelines - test utility for slot timeline following"

EXTENSION = test_slot_timelines
DATA = test_slot_timelines--1.0.sql

EXTRA_INSTALL=contrib/test_decoding
REGRESS=load_extension
REGRESS_OPTS = --temp-config=$(top_srcdir)/src/test/modules/test_slot_timelines/test_slot_timelines.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_slot_timelines
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
