#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/postmaster
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/postmaster/Makefile,v 1.23 2007/07/24 04:54:09 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = autovacuum.o bgwriter.o fork_process.o pgarch.o pgstat.o postmaster.o \
	syslogger.o walwriter.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
