# src/test/modules/test_cplusplusext/Makefile

MODULE_big = test_cplusplusext
OBJS = \
	$(WIN32RES) \
	test_cplusplusext.o
PGFILEDESC = "test_cplusplusext - test C++ compatibility of PostgreSQL headers"

EXTENSION = test_cplusplusext
DATA = test_cplusplusext--1.0.sql

REGRESS = test_cplusplusext

# Use C++ compiler for linking because this module includes C++ files
override COMPILER = $(CXX) $(CXXFLAGS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_cplusplusext
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
