#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/Makefile,v 1.5 1998/04/06 00:23:04 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: submake SUBSYS.o

OBJS = path/SUBSYS.o plan/SUBSYS.o prep/SUBSYS.o util/SUBSYS.o geqo/SUBSYS.o

DIRS = path plan prep util geqo

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep depend
submake:
	for i in $(DIRS); do $(MAKE) -C $$i SUBSYS.o; done

clean:
	rm -f SUBSYS.o
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

.DEFAULT: 
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
