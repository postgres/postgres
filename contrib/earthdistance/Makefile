# $PostgreSQL: pgsql/contrib/earthdistance/Makefile,v 1.14 2004/08/20 20:13:03 momjian Exp $

MODULES = earthdistance
DATA_built = earthdistance.sql
DOCS = README.earthdistance
REGRESS = earthdistance

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/earthdistance
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
