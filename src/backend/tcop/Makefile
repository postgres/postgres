#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tcop
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tcop/Makefile,v 1.1 1996/10/27 09:52:53 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../include \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = aclchk.o dest.o fastpath.o postgres.o pquery.o utility.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

utility.o: ../parse.h

../parse.h: 
	make -C .. parse.h

dep: ../parse.h
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

