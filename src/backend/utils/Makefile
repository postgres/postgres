#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/Makefile,v 1.3 1996/11/03 09:28:31 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../include \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

all: submake SUBSYS.o

OBJS = fmgrtab.o adt/SUBSYS.o cache/SUBSYS.o error/SUBSYS.o \
       fmgr/SUBSYS.o hash/SUBSYS.o init/SUBSYS.o misc/SUBSYS.o mmgr/SUBSYS.o \
       sort/SUBSYS.o time/SUBSYS.o

SUBSYS.o: $(OBJS) 
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep
submake:
	$(MAKE) -C adt SUBSYS.o
	$(MAKE) -C cache SUBSYS.o
	$(MAKE) -C error SUBSYS.o
	$(MAKE) -C fmgr SUBSYS.o
	$(MAKE) -C hash SUBSYS.o
	$(MAKE) -C init SUBSYS.o
	$(MAKE) -C misc SUBSYS.o
	$(MAKE) -C mmgr SUBSYS.o
	$(MAKE) -C sort SUBSYS.o
	$(MAKE) -C time SUBSYS.o

fmgrtab.o: ../fmgr.h

../fmgr.h: 
	$(MAKE) -C .. fmgr.h

fmgr.h fmgrtab.c: ./Gen_fmgrtab.sh ../../include/catalog/pg_proc.h
	sh $(SHOPTS) Gen_fmgrtab.sh ../../include/catalog/pg_proc.h

clean:
	rm -f SUBSYS.o fmgr.h fmgrtab.o fmgrtab.c
	$(MAKE) -C adt   clean 
	$(MAKE) -C cache clean 
	$(MAKE) -C error clean 
	$(MAKE) -C fmgr  clean 
	$(MAKE) -C hash  clean 
	$(MAKE) -C init  clean 
	$(MAKE) -C misc  clean 
	$(MAKE) -C mmgr  clean 
	$(MAKE) -C sort  clean 
	$(MAKE) -C time  clean 

dep: fmgr.h fmgrtab.c
	$(CC) -MM $(INCLUDE_OPT) *.c >depend
	$(MAKE) -C adt   dep 
	$(MAKE) -C cache dep 
	$(MAKE) -C error dep 
	$(MAKE) -C fmgr  dep 
	$(MAKE) -C hash  dep 
	$(MAKE) -C init  dep 
	$(MAKE) -C misc  dep 
	$(MAKE) -C mmgr  dep 
	$(MAKE) -C sort  dep 
	$(MAKE) -C time  dep 

ifeq (depend,$(wildcard depend))
include depend
endif

