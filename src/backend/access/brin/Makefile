#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/brin
#
# IDENTIFICATION
#    src/backend/access/brin/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/brin
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = brin.o brin_pageops.o brin_revmap.o brin_tuple.o brin_xlog.o \
       brin_minmax.o brin_inclusion.o brin_validate.o

include $(top_srcdir)/src/backend/common.mk
