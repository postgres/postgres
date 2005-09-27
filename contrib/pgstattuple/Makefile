#-------------------------------------------------------------------------
#
# pgstattuple Makefile
#
# $PostgreSQL: pgsql/contrib/pgstattuple/Makefile,v 1.4 2005/09/27 17:13:09 tgl Exp $
#
#-------------------------------------------------------------------------

SRCS		= pgstattuple.c

MODULE_big	= pgstattuple
OBJS		= $(SRCS:.c=.o)
DOCS		= README.pgstattuple README.pgstattuple.euc_jp
DATA_built	= pgstattuple.sql

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pgstattuple
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

