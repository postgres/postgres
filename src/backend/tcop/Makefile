#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tcop
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tcop/Makefile,v 1.7 1996/11/14 07:33:34 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

INCLUDE_OPT= -I.. \
             -I../port/$(PORTNAME) \
             -I../../include

CFLAGS+= $(INCLUDE_OPT)

ifeq ($(CC), gcc)
# Until we figure out how to get rid of the warnings in this directory,
# we must turn off any -Werror that is in CFLAGS now.  These options only
# exist for the gcc compiler.
CFLAGS+= -Wno-error
endif

OBJS= aclchk.o dest.o fastpath.o postgres.o pquery.o utility.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

utility.o: ../parse.h

../parse.h: 
	$(MAKE) -C .. parse.h

dep: ../parse.h
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

