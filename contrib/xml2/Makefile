# $PostgreSQL: pgsql/contrib/xml2/Makefile,v 1.14 2010/03/01 18:07:59 tgl Exp $

MODULE_big = pgxml

OBJS = xpath.o xslt_proc.o

SHLIB_LINK += $(filter -lxslt, $(LIBS)) $(filter -lxml2, $(LIBS))

DATA_built = pgxml.sql
DATA = uninstall_pgxml.sql
REGRESS = xml2

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
