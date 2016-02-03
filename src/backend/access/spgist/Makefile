#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/spgist
#
# IDENTIFICATION
#    src/backend/access/spgist/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/spgist
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = spgutils.o spginsert.o spgscan.o spgvacuum.o spgvalidate.o \
	spgdoinsert.o spgxlog.o \
	spgtextproc.o spgquadtreeproc.o spgkdtreeproc.o

include $(top_srcdir)/src/backend/common.mk
