# $PostgreSQL: pgsql/contrib/unaccent/Makefile,v 1.1 2009/08/18 10:34:39 teodor Exp $

MODULE_big = unaccent
OBJS = unaccent.o

DATA_built = unaccent.sql
DATA = uninstall_unaccent.sql
DATA_TSEARCH = unaccent.rules
REGRESS = unaccent


ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_trgm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

#redefine REGRESS_OPTS because of needings of UTF8 database
REGRESS_OPTS = --dbname=$(CONTRIB_TESTDB) --multibyte=UTF8 --no-locale 
