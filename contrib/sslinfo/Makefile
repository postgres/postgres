# $PostgreSQL: pgsql/contrib/sslinfo/Makefile,v 1.9 2007/06/26 22:05:03 tgl Exp $

MODULE_big = sslinfo
OBJS = sslinfo.o
DATA_built = sslinfo.sql
DATA = uninstall_sslinfo.sql
DOCS = README.sslinfo

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/sslinfo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lssl -lcrypto -lssleay32 -leay32, $(LIBS))
