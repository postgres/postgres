# $PostgreSQL: pgsql/contrib/isn/Makefile,v 1.1 2006/09/09 04:07:52 tgl Exp $

MODULES = isn
DATA_built = isn.sql
DATA = uninstall_isn.sql
DOCS = README.isn

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/isn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
