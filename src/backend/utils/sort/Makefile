#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/sort
#
# IDENTIFICATION
#    src/backend/utils/sort/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/sort
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	logtape.o \
	sharedtuplestore.o \
	sortsupport.o \
	tuplesort.o \
	tuplestore.o

include $(top_srcdir)/src/backend/common.mk
