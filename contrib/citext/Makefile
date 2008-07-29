# $PostgreSQL: pgsql/contrib/citext/Makefile,v 1.1 2008/07/29 18:31:20 tgl Exp $

MODULES = citext
DATA_built = citext.sql
DATA = uninstall_citext.sql
REGRESS = citext

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/citext
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
