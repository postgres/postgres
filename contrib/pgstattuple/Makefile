#-------------------------------------------------------------------------
#
# pgstattuple Makefile
#
# $PostgreSQL: pgsql/contrib/pgstattuple/Makefile,v 1.10 2009/04/28 17:07:50 momjian Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pgstattuple
OBJS		= pgstattuple.o pgstatindex.o
DATA_built	= pgstattuple.sql
DATA      	= uninstall_pgstattuple.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgstattuple
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
