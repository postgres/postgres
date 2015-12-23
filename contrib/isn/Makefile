# contrib/isn/Makefile

MODULES = isn

EXTENSION = isn
DATA = isn--1.0.sql isn--unpackaged--1.0.sql
PGFILEDESC = "isn - data types for international product numbering standards"

REGRESS = isn

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/isn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
