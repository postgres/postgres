#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tcop
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/tcop/Makefile,v 1.18 1998/07/26 04:30:46 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

CFLAGS+= -I..

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

ifeq ($(CC), gcc)
CFLAGS+= -Wno-error
endif

OBJS= dest.o fastpath.o postgres.o pquery.o utility.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

# The following dependencies are picked up by the make dep, but since 
# not everyone can do make dep, and these are particularly important
# dependencies (because they don't even exist until you make them),
# they are hardcoded here.

utility.o: ../parse.h ../fmgr.h
aclchk.o: ../fmgr.h
fastpath.o: ../fmgr.h
postgres.o: ../fmgr.h

../parse.h: 
	$(MAKE) -C .. parse.h

../fmgr.h: 
	$(MAKE) -C .. fmgr.h

dep depend: ../parse.h ../fmgr.h
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

