#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/port/win32
#
# IDENTIFICATION
#    src/backend/port/win32/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/port/win32
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	signal.o \
	socket.o \
	timer.o
ifeq ($(have_win32_dbghelp), yes)
OBJS += crashdump.o
endif

include $(top_srcdir)/src/backend/common.mk
