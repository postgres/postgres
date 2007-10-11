# $PostgreSQL: pgsql/contrib/txid/Makefile,v 1.2 2007/10/11 19:54:17 tgl Exp $

MODULES = txid
DATA_built = txid.sql
DATA = uninstall_txid.sql
DOCS = README.txid
REGRESS = txid

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/txid
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
