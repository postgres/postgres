#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/adt
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/adt/Makefile,v 1.4 1997/03/04 05:32:04 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
              -I../../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = acl.o arrayfuncs.o arrayutils.o bool.o char.o chunk.o date.o \
	datum.o dt.o filename.o float.o geo-ops.o geo-selfuncs.o int.o \
	misc.o nabstime.o name.o not_in.o numutils.o oid.o \
	oidname.o oidint2.o oidint4.o oracle_compat.o \
	regexp.o regproc.o selfuncs.o \
	tid.o varchar.o varlena.o sets.o datetimes.o like.o

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

