# contrib/test_parser/Makefile

MODULE_big = test_parser
OBJS = test_parser.o

EXTENSION = test_parser
DATA = test_parser--1.0.sql test_parser--unpackaged--1.0.sql

REGRESS = test_parser

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_parser
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
