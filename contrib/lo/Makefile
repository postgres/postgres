# $PostgreSQL: pgsql/contrib/lo/Makefile,v 1.13 2004/08/20 20:13:05 momjian Exp $

MODULES = lo
DATA_built = lo.sql
DATA = lo_drop.sql lo_test.sql
DOCS = README.lo

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/lo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
