#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/postmaster
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/postmaster/Makefile,v 1.24 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = autovacuum.o bgwriter.o fork_process.o pgarch.o pgstat.o postmaster.o \
	syslogger.o walwriter.o

include $(top_srcdir)/src/backend/common.mk
