#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/page
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/page/Makefile,v 1.1 1996/10/27 09:52:34 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
              -I../../include \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS =  bufpage.o itemptr.o

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

