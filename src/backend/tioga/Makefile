#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tioga
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tioga/Attic/Makefile,v 1.3 1996/11/09 06:22:42 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = tgRecipe.o Varray.o

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

