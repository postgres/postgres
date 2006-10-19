#-------------------------------------------------------------------------
#
# pgrowlocks Makefile
#
# $PostgreSQL: pgsql/contrib/pgrowlocks/Makefile,v 1.3 2006/10/19 17:40:03 tgl Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pgrowlocks
OBJS		= pgrowlocks.o
DOCS		= README.pgrowlocks README.pgrowlocks.euc_jp
DATA_built	= pgrowlocks.sql
DATA		= uninstall_pgrowlocks.sql

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pgrowlocks
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

