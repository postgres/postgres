# contrib/dict_xsyn/Makefile

MODULE_big = dict_xsyn
OBJS = dict_xsyn.o $(WIN32RES)

EXTENSION = dict_xsyn
DATA = dict_xsyn--1.0.sql dict_xsyn--unpackaged--1.0.sql
DATA_TSEARCH = xsyn_sample.rules
PGFILEDESC = "dict_xsyn - add-on dictionary template for full-text search"

REGRESS = dict_xsyn

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/dict_xsyn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
