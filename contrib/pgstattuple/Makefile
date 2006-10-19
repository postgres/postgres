#-------------------------------------------------------------------------
#
# pgstattuple Makefile
#
# $PostgreSQL: pgsql/contrib/pgstattuple/Makefile,v 1.7 2006/10/19 17:40:03 tgl Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pgstattuple
OBJS		= pgstattuple.o pgstatindex.o
DOCS		= README.pgstattuple README.pgstattuple.euc_jp
DATA_built	= pgstattuple.sql
DATA      	= uninstall_pgstattuple.sql

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pgstattuple
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

