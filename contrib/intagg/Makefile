#############################################
# Makefile for integer aggregator
# Copyright (C) 2001 Digital Music Network.
# by Mark L. Woodward
# $PostgreSQL: pgsql/contrib/intagg/Makefile,v 1.9 2007/11/10 23:59:51 momjian Exp $

MODULES = int_aggregate
DATA_built = int_aggregate.sql
DATA = uninstall_int_aggregate.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/intagg
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
