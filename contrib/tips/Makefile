# $PostgreSQL: pgsql/contrib/tips/Makefile,v 1.8 2005/09/27 17:13:10 tgl Exp $

DOCS = README.apachelog

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tips
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
