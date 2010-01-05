#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/cache
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/cache/Makefile,v 1.24 2010/01/05 21:53:59 rhaas Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = catcache.o inval.o plancache.o relcache.o \
	spccache.o syscache.o lsyscache.o typcache.o ts_cache.o

include $(top_srcdir)/src/backend/common.mk
