#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/rtree
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/Makefile,v 1.4 1997/12/17 04:31:24 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

ifdef PORTNAME
INCLUDE_OPT += -I../../port/$(PORTNAME) 
endif

CFLAGS += $(INCLUDE_OPT)

OBJS = rtget.o rtproc.o rtree.o rtscan.o rtstrat.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

