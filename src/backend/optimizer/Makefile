#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/Makefile,v 1.1 1996/10/27 09:48:14 bryanh Exp $
#
#-------------------------------------------------------------------------

all: SUBSYS.o

SUBSYS.o: 
	$(MAKE) -C path SUBSYS.o
	$(MAKE) -C plan SUBSYS.o
	$(MAKE) -C prep SUBSYS.o
	$(MAKE) -C util SUBSYS.o
	$(LD) -r -o SUBSYS.o \
	     path/SUBSYS.o plan/SUBSYS.o prep/SUBSYS.o util/SUBSYS.o 

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
