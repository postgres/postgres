#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for postmaster
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/postmaster/Makefile,v 1.4 1996/12/28 02:12:04 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

ifeq ($(PORTNAME), sparc_solaris)

ifeq ($(CC), gcc)
# Until we figure out how to get rid of the warnings in this directory,
# we must turn off any -Werror that is in CFLAGS now.  These options only
# exist for the gcc compiler.
CFLAGS+= -Wno-error
endif

endif

OBJS = postmaster.o

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

