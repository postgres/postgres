# $PostgreSQL: pgsql/contrib/tablefunc/Makefile,v 1.9 2007/11/10 23:59:51 momjian Exp $

MODULES = tablefunc
DATA_built = tablefunc.sql
DATA = uninstall_tablefunc.sql
REGRESS = tablefunc

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tablefunc
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
