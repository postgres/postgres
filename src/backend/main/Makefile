#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for main
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/main/Makefile,v 1.4 1997/12/20 00:24:03 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. 

CFLAGS+=$(INCLUDE_OPT)

OBJS = main.o

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

