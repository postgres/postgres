# $PostgreSQL: pgsql/contrib/auto_explain/Makefile,v 1.1 2008/11/19 02:59:28 tgl Exp $

MODULE_big = auto_explain
OBJS = auto_explain.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/auto_explain
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
