#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tioga
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tioga/Attic/Makefile,v 1.7 2000/05/29 05:45:18 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

OBJS = tgRecipe.o Varray.o

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

