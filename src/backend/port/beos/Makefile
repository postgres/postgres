#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/beos
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/beos/Makefile,v 1.4 2003/11/29 19:51:54 pgsql Exp $
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
