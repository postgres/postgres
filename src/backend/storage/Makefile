#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the storage manager subsystem
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/Makefile,v 1.5 1999/12/09 19:14:58 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

all: submake SUBSYS.o

OBJS = buffer/SUBSYS.o file/SUBSYS.o ipc/SUBSYS.o large_object/SUBSYS.o \
       lmgr/SUBSYS.o page/SUBSYS.o smgr/SUBSYS.o

DIRS = buffer file ipc large_object lmgr page smgr

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep

submake:
	for i in $(DIRS); do $(MAKE) -C $$i SUBSYS.o; done

clean:
	rm -f SUBSYS.o
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

.DEFAULT: 
	for i in $(DIRS); do $(MAKE) -C $$i $@; done
