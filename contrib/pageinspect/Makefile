#-------------------------------------------------------------------------
#
# pageinspect Makefile
#
# $PostgreSQL: pgsql/contrib/pageinspect/Makefile,v 1.1 2007/05/17 19:11:24 momjian Exp $
#
#-------------------------------------------------------------------------

MODULE_big	= pageinspect
OBJS		= rawpage.o heapfuncs.o btreefuncs.o
DOCS		= README.pageinspect
DATA_built	= pageinspect.sql
DATA      	= uninstall_pageinspect.sql

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pageinspect
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

