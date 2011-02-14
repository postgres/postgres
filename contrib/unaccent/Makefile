# contrib/unaccent/Makefile

MODULE_big = unaccent
OBJS = unaccent.o

EXTENSION = unaccent
DATA = unaccent--1.0.sql unaccent--unpackaged--1.0.sql
DATA_TSEARCH = unaccent.rules

REGRESS = unaccent

# Adjust REGRESS_OPTS because we need a UTF8 database
REGRESS_OPTS = --dbname=$(CONTRIB_TESTDB) --multibyte=UTF8 --no-locale

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/unaccent
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
