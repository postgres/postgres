# $PostgreSQL: pgsql/contrib/dict_int/Makefile,v 1.1 2007/10/15 21:36:50 tgl Exp $

MODULE_big = dict_int
OBJS = dict_int.o
DATA_built = dict_int.sql
DATA = uninstall_dict_int.sql
DOCS = README.dict_int
REGRESS = dict_int

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/dict_int
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
