#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/path
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/path/Makefile,v 1.13 2000/02/07 04:40:59 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

OBJS = allpaths.o clausesel.o costsize.o indxpath.o \
       joinpath.o joinrels.o orindxpath.o pathkeys.o tidpath.o

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

