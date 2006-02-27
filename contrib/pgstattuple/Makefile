#-------------------------------------------------------------------------
#
# pgstattuple Makefile
#
# $PostgreSQL: pgsql/contrib/pgstattuple/Makefile,v 1.5 2006/02/27 12:54:39 petere Exp $
#
#-------------------------------------------------------------------------

SRCS		= pgstattuple.c

MODULE_big	= pgstattuple
OBJS		= $(SRCS:.c=.o)
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

