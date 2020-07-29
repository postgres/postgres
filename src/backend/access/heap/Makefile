#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/heap
#
# IDENTIFICATION
#    src/backend/access/heap/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/heap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	heapam.o \
	heapam_handler.o \
	heapam_visibility.o \
	heaptoast.o \
	hio.o \
	pruneheap.o \
	rewriteheap.o \
	vacuumlazy.o \
	visibilitymap.o

include $(top_srcdir)/src/backend/common.mk
