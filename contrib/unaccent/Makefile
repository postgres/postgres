# $PostgreSQL: pgsql/contrib/unaccent/Makefile,v 1.2 2009/08/18 15:51:16 tgl Exp $

MODULE_big = unaccent
OBJS = unaccent.o

DATA_built = unaccent.sql
DATA = uninstall_unaccent.sql
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
