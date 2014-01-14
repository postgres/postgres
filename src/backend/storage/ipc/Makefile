#
# Makefile for storage/ipc
#
# src/backend/storage/ipc/Makefile
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

OBJS = dsm_impl.o dsm.o ipc.o ipci.o pmsignal.o procarray.o procsignal.o \
	shmem.o shmqueue.o shm_mq.o shm_toc.o sinval.o sinvaladt.o standby.o

include $(top_srcdir)/src/backend/common.mk
