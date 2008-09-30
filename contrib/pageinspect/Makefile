#-------------------------------------------------------------------------
#
# pageinspect Makefile
#
# $PostgreSQL: pgsql/contrib/pageinspect/Makefile,v 1.4 2008/09/30 10:52:09 heikki Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pageinspect
OBJS		= rawpage.o heapfuncs.o btreefuncs.o fsmfuncs.o
DATA_built	= pageinspect.sql
DATA      	= uninstall_pageinspect.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pageinspect
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

