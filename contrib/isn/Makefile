# $PostgreSQL: pgsql/contrib/isn/Makefile,v 1.3 2007/11/10 23:59:51 momjian Exp $

MODULES = isn
DATA_built = isn.sql
DATA = uninstall_isn.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/isn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
