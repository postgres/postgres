#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/postmaster
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/postmaster/Makefile,v 1.22 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = bgwriter.o autovacuum.o pgarch.o pgstat.o postmaster.o syslogger.o \
	fork_process.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
