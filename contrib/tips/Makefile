# $PostgreSQL: pgsql/contrib/tips/Makefile,v 1.7 2004/08/20 20:13:08 momjian Exp $

DOCS = README.apachelog

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tips
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
