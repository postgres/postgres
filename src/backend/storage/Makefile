#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the storage manager subsystem
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/Makefile,v 1.2 1996/11/10 03:13:59 bryanh Exp $
#
#-------------------------------------------------------------------------

all: submake SUBSYS.o

OBJS = buffer/SUBSYS.o file/SUBSYS.o ipc/SUBSYS.o large_object/SUBSYS.o \
       lmgr/SUBSYS.o page/SUBSYS.o smgr/SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep

submake:
	$(MAKE) -C buffer  SUBSYS.o
	$(MAKE) -C file    SUBSYS.o
	$(MAKE) -C ipc     SUBSYS.o
	$(MAKE) -C large_object SUBSYS.o
	$(MAKE) -C lmgr    SUBSYS.o
	$(MAKE) -C page    SUBSYS.o
	$(MAKE) -C smgr    SUBSYS.o

clean:
	rm -f SUBSYS.o
	$(MAKE) -C buffer   clean
	$(MAKE) -C file     clean
	$(MAKE) -C ipc      clean
	$(MAKE) -C large_object clean
	$(MAKE) -C lmgr     clean
	$(MAKE) -C page     clean
	$(MAKE) -C smgr     clean

.DEFAULT: 
	$(MAKE) -C buffer   $@
	$(MAKE) -C file     $@
	$(MAKE) -C ipc      $@
	$(MAKE) -C large_object $@
	$(MAKE) -C lmgr     $@
	$(MAKE) -C page     $@
	$(MAKE) -C smgr     $@

