#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/cache
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/cache/Makefile,v 1.26 2010/02/07 20:48:10 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = attoptcache.o catcache.o inval.o plancache.o relcache.o relmapper.o \
	spccache.o syscache.o lsyscache.o typcache.o ts_cache.o

include $(top_srcdir)/src/backend/common.mk
