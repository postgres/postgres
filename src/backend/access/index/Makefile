#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/index
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/index/Makefile,v 1.3 1996/11/05 10:02:01 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

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

