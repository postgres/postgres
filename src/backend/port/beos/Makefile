#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/beos
#
#-------------------------------------------------------------------------

top_builddir = ../../../..
include ../../../Makefile.global

INCLUDE_OPT = 

CFLAGS+=$(INCLUDE_OPT)

OBJS = sem.o shm.o support.o

all: SUBSYS.o 

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

