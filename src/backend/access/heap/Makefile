#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/heap
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/heap/Makefile,v 1.19 2008/12/03 13:05:22 heikki Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/heap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heapam.o hio.o pruneheap.o rewriteheap.o syncscan.o tuptoaster.o visibilitymap.o

include $(top_srcdir)/src/backend/common.mk
