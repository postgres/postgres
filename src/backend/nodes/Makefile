#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/nodes
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/nodes/Makefile,v 1.20 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/nodes
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = nodeFuncs.o nodes.o list.o bitmapset.o tidbitmap.o \
       copyfuncs.o equalfuncs.o makefuncs.o \
       outfuncs.o readfuncs.o print.o read.o params.o value.o

include $(top_srcdir)/src/backend/common.mk
