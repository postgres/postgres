#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/Makefile,v 1.7 1998/07/24 03:31:39 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. 

CFLAGS+=$(INCLUDE_OPT)

all: submake SUBSYS.o

OBJS = fmgrtab.o adt/SUBSYS.o cache/SUBSYS.o error/SUBSYS.o \
       fmgr/SUBSYS.o hash/SUBSYS.o init/SUBSYS.o misc/SUBSYS.o mmgr/SUBSYS.o \
       sort/SUBSYS.o time/SUBSYS.o

DIRS = adt cache error fmgr hash init misc mmgr sort time

ifdef MB
OBJS += mb/SUBSYS.o
DIRS += mb
endif

SUBSYS.o: $(OBJS) 
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep
submake:
	for i in $(DIRS); do $(MAKE) -C $$i SUBSYS.o; done

fmgrtab.o: ../fmgr.h

../fmgr.h: 
	$(MAKE) -C .. fmgr.h

fmgr.h fmgrtab.c: ./Gen_fmgrtab.sh ../../include/catalog/pg_proc.h
	sh $(SHOPTS) Gen_fmgrtab.sh ../../include/catalog/pg_proc.h

clean:
	rm -f SUBSYS.o fmgr.h fmgrtab.o fmgrtab.c
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

dep depend: fmgr.h fmgrtab.c
	for i in $(DIRS); do $(MAKE) -C $$i depend; done

ifeq (depend,$(wildcard depend))
include depend
endif

