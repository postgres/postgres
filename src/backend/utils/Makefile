#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/Makefile,v 1.13 2000/06/07 16:26:48 petere Exp $
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

.PHONY: submake clean dep
submake:
	for i in $(DIRS); do $(MAKE) -C $$i SUBSYS.o; done

# Gen_fmgrtab.sh will not change the timestamp of its output files
# if they already exist and would not be changed. This is to avoid
# unnecessary recompilations. In order to avoid re-running it all
# the time we update a stamp file instead. (Idea stolen from
# autoconf and autoheader.)

fmgroids.h fmgrtab.c: fmgrstamp-h
fmgrstamp-h: Gen_fmgrtab.sh $(SRCDIR)/include/catalog/pg_proc.h
	$(SHELL) $(SHOPTS) Gen_fmgrtab.sh $(SRCDIR)/include/catalog/pg_proc.h
	echo timestamp > fmgrstamp-h

# don't clean fmgroids.h and fmgrtab.c
clean:
	rm -f SUBSYS.o fmgrtab.o
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

dep depend: fmgroids.h fmgrtab.c
	for i in $(DIRS); do $(MAKE) -C $$i depend; done

ifeq (depend,$(wildcard depend))
include depend
endif

