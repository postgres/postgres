#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/misc
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/misc/Makefile,v 1.4 1997/12/19 02:08:42 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../..

ifdef PORTNAME
INCLUDE+=-I../../port/$(PORTNAME)    
endif

CFLAGS += $(INCLUDE_OPT)

OBJS = database.o superuser.o 

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

