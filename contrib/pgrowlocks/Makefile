#-------------------------------------------------------------------------
#
# pgrowlocks Makefile
#
# $PostgreSQL: pgsql/contrib/pgrowlocks/Makefile,v 1.4 2007/06/26 22:05:03 tgl Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pgrowlocks
OBJS		= pgrowlocks.o
DOCS		= README.pgrowlocks README.pgrowlocks.euc_jp
DATA_built	= pgrowlocks.sql
DATA		= uninstall_pgrowlocks.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgrowlocks
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

