#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for postmaster
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/postmaster/Makefile,v 1.6 1997/12/19 02:06:34 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. 

ifdef PORTNAME
INCLUDE_OPT+=-I../port/$(PORTNAME) 
endif

CFLAGS+=$(INCLUDE_OPT)

OBJS = postmaster.o

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

