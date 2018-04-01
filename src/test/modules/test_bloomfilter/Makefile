# src/test/modules/test_bloomfilter/Makefile

MODULE_big = test_bloomfilter
OBJS = test_bloomfilter.o $(WIN32RES)
PGFILEDESC = "test_bloomfilter - test code for Bloom filter library"

EXTENSION = test_bloomfilter
DATA = test_bloomfilter--1.0.sql

REGRESS = test_bloomfilter

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_bloomfilter
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
