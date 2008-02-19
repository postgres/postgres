#
# Makefile for storage/ipc
#
# $PostgreSQL: pgsql/src/backend/storage/ipc/Makefile,v 1.21 2008/02/19 10:30:08 petere Exp $
#

subdir = src/backend/storage/ipc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# seems to be required 1999/07/22 bjm
ifeq "$(findstring alpha,$(host_cpu))" "alpha"
ifeq "$(GCC)" "yes"
override CFLAGS+= -fno-inline
endif
endif

OBJS = ipc.o ipci.o pmsignal.o procarray.o shmem.o shmqueue.o \
	sinval.o sinvaladt.o

include $(top_srcdir)/src/backend/common.mk
