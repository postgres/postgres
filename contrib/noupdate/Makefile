# $PostgreSQL: pgsql/contrib/noupdate/Makefile,v 1.11 2004/08/20 20:13:05 momjian Exp $

MODULES = noup
DATA_built = noup.sql
DOCS = README.noup

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/noupdate
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
