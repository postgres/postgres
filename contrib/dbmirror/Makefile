# $PostgreSQL: pgsql/contrib/dbmirror/Makefile,v 1.3 2004/08/20 20:13:03 momjian Exp $

MODULES = pending
DOCS = README.dbmirror

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/dbmirror
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
