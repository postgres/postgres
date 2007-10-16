# $PostgreSQL: pgsql/contrib/dict_xsyn/Makefile,v 1.2 2007/10/16 15:59:59 tgl Exp $

MODULE_big = dict_xsyn
OBJS = dict_xsyn.o
DATA_built = dict_xsyn.sql
DATA = uninstall_dict_xsyn.sql
DATA_TSEARCH = xsyn_sample.rules
DOCS = README.dict_xsyn
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
