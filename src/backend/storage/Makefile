#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the storage manager subsystem
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/Makefile,v 1.4 1998/04/06 00:24:53 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: submake SUBSYS.o

OBJS = buffer/SUBSYS.o file/SUBSYS.o ipc/SUBSYS.o large_object/SUBSYS.o \
       lmgr/SUBSYS.o page/SUBSYS.o smgr/SUBSYS.o

DIRS = buffer file ipc large_object lmgr page smgr

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep

submake:
	for i in $(DIRS); do $(MAKE) -C $$i SUBSYS.o; done

clean:
	rm -f SUBSYS.o
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

.DEFAULT: 
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
