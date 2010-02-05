# $PostgreSQL: pgsql/contrib/intagg/Makefile,v 1.11 2010/02/05 03:06:30 momjian Exp $

DATA = int_aggregate.sql uninstall_int_aggregate.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/intagg
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
