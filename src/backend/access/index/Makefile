#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/index
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/index/Makefile,v 1.4 1997/12/17 04:31:11 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

ifdef PORTNAME
INCLUDE_OPT += -I../../port/$(PORTNAME) 
endif

CFLAGS += $(INCLUDE_OPT)

OBJS = genam.o indexam.o istrat.o

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

