# $Header: /cvsroot/pgsql/src/backend/port/beos/Attic/Makefile,v 1.2 2000/10/09 16:42:54 petere Exp $

subdir = src/backend/port/beos
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = sem.o shm.o support.o

all: SUBSYS.o 

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

depend dep:
	$(CC) -MM $(CFLAGS) $(CPPFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif
