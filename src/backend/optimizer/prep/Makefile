#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/prep
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/prep/Makefile,v 1.1 1996/10/27 09:48:46 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
              -I../../include \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = archive.o prepqual.o preptlist.o prepunion.o

# not ready yet: predmig.o xfunc.o

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

