# contrib/fuzzystrmatch/Makefile

MODULE_big = fuzzystrmatch
OBJS = fuzzystrmatch.o dmetaphone.o

EXTENSION = fuzzystrmatch
DATA = fuzzystrmatch--1.0.sql fuzzystrmatch--unpackaged--1.0.sql

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

# levenshtein.c is #included by fuzzystrmatch.c
fuzzystrmatch.o: fuzzystrmatch.c levenshtein.c
