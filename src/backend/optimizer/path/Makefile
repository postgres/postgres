#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/path
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/path/Makefile,v 1.4 1997/12/19 02:05:59 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

ifdef PORTNAME
INCLUDE_OPT+=-I../../port/$(PORTNAME)  
endif

CFLAGS+=$(INCLUDE_OPT)

OBJS = allpaths.o clausesel.o costsize.o hashutils.o indxpath.o \
       joinpath.o joinrels.o joinutils.o mergeutils.o orindxpath.o \
       prune.o

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

