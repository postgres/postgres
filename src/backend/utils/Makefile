#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/Makefile,v 1.14 2000/06/09 02:38:36 tgl Exp $
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

# Gen_fmgrtab.sh will not change the timestamp of its output files
# if they already exist and would not be changed. This is to avoid
# unnecessary recompilations. In order to avoid re-running it all
# the time we update a stamp file instead. (Idea stolen from
# autoconf and autoheader.)

fmgroids.h fmgrtab.c: fmgrstamp-h

fmgrstamp-h: Gen_fmgrtab.sh $(SRCDIR)/include/catalog/pg_proc.h
	$(SHELL) $(SHOPTS) Gen_fmgrtab.sh $(SRCDIR)/include/catalog/pg_proc.h
	date > fmgrstamp-h

# don't clean fmgroids.h and fmgrtab.c, but do clean fmgrstamp-h
# (we don't really want to put that much trust in timestamps in
# distribution files and CVS pulls, so force at least one run of
# Gen_fmgrtab.sh after a make clean)

clean:
	rm -f SUBSYS.o fmgrtab.o fmgrstamp-h
	for i in $(DIRS); do $(MAKE) -C $$i clean; done

dep depend: fmgroids.h fmgrtab.c
	for i in $(DIRS); do $(MAKE) -C $$i depend; done

ifeq (depend,$(wildcard depend))
include depend
endif
