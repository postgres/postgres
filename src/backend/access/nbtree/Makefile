#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/nbtree
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/nbtree/Makefile,v 1.6 1998/04/06 00:21:34 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../.. 

OBJS = nbtcompare.o nbtinsert.o nbtpage.o nbtree.o nbtscan.o nbtsearch.o \
       nbtstrat.o nbtutils.o nbtsort.o

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

