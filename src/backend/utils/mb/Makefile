#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    src/backend/utils/mb/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	conv.o \
	mbutils.o \
	stringinfo_mb.o \
	wstrcmp.o \
	wstrncmp.o

include $(top_srcdir)/src/backend/common.mk

clean distclean maintainer-clean:
	$(MAKE) -C conversion_procs $@
