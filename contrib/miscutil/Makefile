# $PostgreSQL: pgsql/contrib/miscutil/Makefile,v 1.18 2004/08/20 20:13:05 momjian Exp $

MODULES = misc_utils
DATA_built = misc_utils.sql
DOCS = README.misc_utils

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/miscutil
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
