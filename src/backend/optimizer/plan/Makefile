#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/plan
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/plan/Makefile,v 1.6 1998/02/13 03:36:51 vadim Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. 

CFLAGS+=$(INCLUDE_OPT)

OBJS = createplan.o initsplan.o planmain.o planner.o setrefs.o subselect.o

# not ready yet: predmig.o xfunc.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

