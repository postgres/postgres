#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/Makefile,v 1.16 2000/07/01 21:16:44 petere Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include $(SRCDIR)/Makefile.global

all: submake SUBSYS.o

OBJS = fmgrtab.o adt/SUBSYS.o cache/SUBSYS.o error/SUBSYS.o \
       fmgr/SUBSYS.o hash/SUBSYS.o init/SUBSYS.o misc/SUBSYS.o mmgr/SUBSYS.o \
       sort/SUBSYS.o time/SUBSYS.o

DIRS = adt cache error fmgr hash init misc mmgr sort time

ifdef MULTIBYTE
OBJS += mb/SUBSYS.o
DIRS += mb
endif

SUBSYS.o: $(OBJS) 
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

.PHONY: submake clean dep depend

submake:
	for i in $(DIRS); do $(MAKE) -C $$i SUBSYS.o; done


fmgroids.h fmgrtab.c: Gen_fmgrtab.sh $(SRCDIR)/include/catalog/pg_proc.h
	CPP='$(CPP)' AWK='$(AWK)' $(SHELL) $< $(SRCDIR)/include/catalog/pg_proc.h


clean:
	rm -f SUBSYS.o fmgrtab.o fmgroids.h fmgrtab.c
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

dep depend: fmgroids.h fmgrtab.c
	for i in $(DIRS); do $(MAKE) -C $$i depend; done

ifeq (depend,$(wildcard depend))
include depend
endif
