#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/Makefile,v 1.4 1997/06/11 01:12:55 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: submake SUBSYS.o

OBJS = path/SUBSYS.o plan/SUBSYS.o prep/SUBSYS.o util/SUBSYS.o geqo/SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean
submake:
	$(MAKE) -C path SUBSYS.o
	$(MAKE) -C plan SUBSYS.o
	$(MAKE) -C prep SUBSYS.o
	$(MAKE) -C util SUBSYS.o
	$(MAKE) -C geqo SUBSYS.o

clean:
	rm -f SUBSYS.o
	$(MAKE) -C path clean
	$(MAKE) -C plan clean
	$(MAKE) -C prep clean
	$(MAKE) -C util clean
	$(MAKE) -C geqo clean

.DEFAULT: 
	$(MAKE) -C path $@
	$(MAKE) -C plan $@
	$(MAKE) -C prep $@
	$(MAKE) -C util $@
	$(MAKE) -C geqo $@
