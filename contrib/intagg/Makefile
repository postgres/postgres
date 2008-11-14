#############################################
# Makefile for integer aggregator
# Copyright (C) 2001 Digital Music Network.
# by Mark L. Woodward
# $PostgreSQL: pgsql/contrib/intagg/Makefile,v 1.10 2008/11/14 19:58:45 tgl Exp $

DATA = int_aggregate.sql uninstall_int_aggregate.sql

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
