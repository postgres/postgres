# src/test/modules/test_parser/Makefile

MODULE_big = test_parser
OBJS = \
	$(WIN32RES) \
	test_parser.o
PGFILEDESC = "test_parser - example of a custom parser for full-text search"

EXTENSION = test_parser
DATA = test_parser--1.0.sql

REGRESS = test_parser

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_parser
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
