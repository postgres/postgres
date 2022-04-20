#-------------------------------------------------------------------------
#
# Makefile for backend/utils/activity
#
# Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/utils/activity/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/activity
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	backend_progress.o \
	backend_status.o \
	pgstat.o \
	pgstat_archiver.o \
	pgstat_bgwriter.o \
	pgstat_checkpointer.o \
	pgstat_database.o \
	pgstat_function.o \
	pgstat_relation.o \
	pgstat_replslot.o \
	pgstat_shmem.o \
	pgstat_slru.o \
	pgstat_subscription.o \
	pgstat_wal.o \
	pgstat_xact.o \
	wait_event.o

include $(top_srcdir)/src/backend/common.mk
