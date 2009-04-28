#-------------------------------------------------------------------------
#
# pgrowlocks Makefile
#
# $PostgreSQL: pgsql/contrib/pgrowlocks/Makefile,v 1.6 2009/04/28 17:07:50 momjian Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pgrowlocks
OBJS		= pgrowlocks.o
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
