#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/init
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/init/Makefile,v 1.12 2000/01/19 02:58:57 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = enbl.o findbe.o globals.o miscinit.o postinit.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

