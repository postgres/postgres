#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/Makefile,v 1.2 1996/11/10 03:12:38 bryanh Exp $
#
#-------------------------------------------------------------------------

all: submake SUBSYS.o

OBJS = path/SUBSYS.o plan/SUBSYS.o prep/SUBSYS.o util/SUBSYS.o 

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean
submake:
	$(MAKE) -C path SUBSYS.o
	$(MAKE) -C plan SUBSYS.o
	$(MAKE) -C prep SUBSYS.o
	$(MAKE) -C util SUBSYS.o

clean:
	rm -f SUBSYS.o
	$(MAKE) -C path clean
	$(MAKE) -C plan clean
	$(MAKE) -C prep clean
	$(MAKE) -C util clean

.DEFAULT: 
	$(MAKE) -C path $@
	$(MAKE) -C plan $@
	$(MAKE) -C prep $@
	$(MAKE) -C util $@
