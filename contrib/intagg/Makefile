#############################################
# Makefile for integer aggregator
# Copyright (C) 2001 Digital Music Network.
# by Mark L. Woodward
# $PostgreSQL: pgsql/contrib/intagg/Makefile,v 1.5 2004/08/20 20:13:04 momjian Exp $

MODULES = int_aggregate
DATA_built = int_aggregate.sql
DOCS = README.int_aggregate

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/intagg
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
