subdir = src/gausskernel/optimizer/heuristic
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
    ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
      -include $(DEPEND)
    endif
  endif
endif
OBJS =	heuristic_manager.o

include $(top_srcdir)/src/gausskernel/common.mk


//
#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the genetic query optimizer module
#
# Copyright (c) 1994, Regents of the University of California
#
# src/backend/optimizer/geqo/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/geqo
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	heuristic_manager.o \
  counter.o \
  graph_utils.o \

include $(top_srcdir)/src/backend/common.mk
