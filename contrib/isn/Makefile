# $PostgreSQL: pgsql/contrib/isn/Makefile,v 1.2 2007/06/26 22:05:02 tgl Exp $

MODULES = isn
DATA_built = isn.sql
DATA = uninstall_isn.sql
DOCS = README.isn

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
