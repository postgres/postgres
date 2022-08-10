#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/backup
#
# IDENTIFICATION
#    src/backend/backup/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/backup
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	backup_manifest.o \
	basebackup.o \
	basebackup_copy.o \
	basebackup_gzip.o \
	basebackup_lz4.o \
	basebackup_zstd.o \
	basebackup_progress.o \
	basebackup_server.o \
	basebackup_sink.o \
	basebackup_target.o \
	basebackup_throttle.o

include $(top_srcdir)/src/backend/common.mk
