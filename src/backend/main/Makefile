#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for main
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/main/Makefile,v 1.5 1998/04/06 00:22:51 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

OBJS = main.o

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

