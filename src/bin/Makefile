#-------------------------------------------------------------------------
#
# Makefile for src/bin (client programs)
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/Makefile
#
#-------------------------------------------------------------------------

subdir = src/bin
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
	initdb \
	pg_archivecleanup \
	pg_basebackup \
	pg_config \
	pg_controldata \
	pg_ctl \
	pg_dump \
	pg_resetxlog \
	pg_rewind \
	pg_test_fsync \
	pg_test_timing \
	pg_upgrade \
	pg_xlogdump \
	pgbench \
	psql \
	scripts

ifeq ($(PORTNAME), win32)
SUBDIRS += pgevent
else
ALWAYS_SUBDIRS += pgevent
endif

$(recurse)
$(recurse_always)
