# $PostgreSQL: pgsql/contrib/isbn_issn/Makefile,v 1.14 2005/09/27 17:13:04 tgl Exp $

MODULES = isbn_issn
DATA_built = isbn_issn.sql
DOCS = README.isbn_issn

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/isbn_issn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
