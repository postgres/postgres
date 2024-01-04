#-------------------------------------------------------------------------
#
# Makefile for backend/utils/activity
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/utils/activity/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/activity
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	backend_progress.o \
	backend_status.o \
	pgstat.o \
	pgstat_archiver.o \
	pgstat_bgwriter.o \
	pgstat_checkpointer.o \
	pgstat_database.o \
	pgstat_function.o \
	pgstat_io.o \
	pgstat_relation.o \
	pgstat_replslot.o \
	pgstat_shmem.o \
	pgstat_slru.o \
	pgstat_subscription.o \
	pgstat_wal.o \
	pgstat_xact.o \
	wait_event.o \
	wait_event_funcs.o

include $(top_srcdir)/src/backend/common.mk

wait_event_funcs.o: wait_event_funcs_data.c
wait_event_funcs_data.c: wait_event_types.h

wait_event.o: pgstat_wait_event.c
pgstat_wait_event.c: wait_event_types.h
	touch $@

wait_event_types.h: $(top_srcdir)/src/backend/utils/activity/wait_event_names.txt generate-wait_event_types.pl
	$(PERL) $(srcdir)/generate-wait_event_types.pl --code $<

clean:
	rm -f wait_event_types.h pgstat_wait_event.c wait_event_funcs_data.c
