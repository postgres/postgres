#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/prep
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/prep/Makefile,v 1.7 1998/04/06 00:23:48 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = prepqual.o preptlist.o prepunion.o

# not ready yet: predmig.o xfunc.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

