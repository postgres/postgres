# contrib/fuzzystrmatch/Makefile

MODULE_big = fuzzystrmatch
OBJS = fuzzystrmatch.o dmetaphone.o $(WIN32RES)

EXTENSION = fuzzystrmatch
DATA = fuzzystrmatch--1.1.sql fuzzystrmatch--1.0--1.1.sql \
	fuzzystrmatch--unpackaged--1.0.sql
PGFILEDESC = "fuzzystrmatch - similarities and distance between strings"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/fuzzystrmatch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
