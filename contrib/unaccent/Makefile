# contrib/unaccent/Makefile

MODULE_big = unaccent
OBJS = unaccent.o $(WIN32RES)

EXTENSION = unaccent
DATA = unaccent--1.1.sql unaccent--1.0--1.1.sql unaccent--unpackaged--1.0.sql
DATA_TSEARCH = unaccent.rules
PGFILEDESC = "unaccent - text search dictionary that removes accents"

REGRESS = unaccent

# We need a UTF8 database
ENCODING = UTF8
NO_LOCALE = 1

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
