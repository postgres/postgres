#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/beos
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/beos/Attic/Makefile,v 1.3 2003/04/24 17:16:13 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/beos
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = sem.o shm.o support.o

all: SUBSYS.o 

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) $(CPPFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
