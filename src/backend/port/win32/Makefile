#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/port/win32
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/win32/Makefile,v 1.9 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/win32
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = shmem.o timer.o socket.o signal.o security.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
