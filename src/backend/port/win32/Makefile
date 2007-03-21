#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/port/win32
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/win32/Makefile,v 1.10 2007/03/21 14:39:23 mha Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/win32
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = timer.o socket.o signal.o security.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
