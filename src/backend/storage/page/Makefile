#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/page
#
# IDENTIFICATION
#    src/backend/storage/page/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/page
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS =  \
	bufpage.o \
	checksum.o \
	itemptr.o

include $(top_srcdir)/src/backend/common.mk

# Provide special optimization flags for checksum.c
checksum.o: CFLAGS += ${CFLAGS_UNROLL_LOOPS} ${CFLAGS_VECTORIZE}
