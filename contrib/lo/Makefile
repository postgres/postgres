# $PostgreSQL: pgsql/contrib/lo/Makefile,v 1.15 2006/09/11 15:14:46 tgl Exp $

MODULES = lo
DATA_built = lo.sql
DATA = uninstall_lo.sql
DOCS = README.lo

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/lo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
