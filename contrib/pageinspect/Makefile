#-------------------------------------------------------------------------
#
# pageinspect Makefile
#
# $PostgreSQL: pgsql/contrib/pageinspect/Makefile,v 1.2 2007/06/26 22:05:02 tgl Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pageinspect
OBJS		= rawpage.o heapfuncs.o btreefuncs.o
DOCS		= README.pageinspect
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

