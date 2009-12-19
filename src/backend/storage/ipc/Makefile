#
# Makefile for storage/ipc
#
# $PostgreSQL: pgsql/src/backend/storage/ipc/Makefile,v 1.23 2009/12/19 01:32:35 sriggs Exp $
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

OBJS = ipc.o ipci.o pmsignal.o procarray.o procsignal.o shmem.o shmqueue.o \
	sinval.o sinvaladt.o standby.o

include $(top_srcdir)/src/backend/common.mk
