# $PostgreSQL: pgsql/contrib/xml2/Makefile,v 1.11 2007/11/10 23:59:52 momjian Exp $

MODULE_big = pgxml

# Remove xslt_proc.o from the following line if you don't have libxslt
OBJS = xpath.o xslt_proc.o

# Remove -lxslt from the following line if you don't have libxslt.
SHLIB_LINK = -lxslt -lxml2

DATA_built = pgxml.sql
DATA = uninstall_pgxml.sql

override CFLAGS += $(shell xml2-config --cflags)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/xml2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
