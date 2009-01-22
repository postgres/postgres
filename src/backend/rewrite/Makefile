#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for rewrite
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/rewrite/Makefile,v 1.18 2009/01/22 17:27:54 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/rewrite
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = rewriteRemove.o rewriteDefine.o \
       rewriteHandler.o rewriteManip.o rewriteSupport.o \
       viewUpdate.o

include $(top_srcdir)/src/backend/common.mk
