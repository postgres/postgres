#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/adt
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/adt/Makefile,v 1.13 1998/04/29 12:38:01 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

ifdef MB
CFLAGS+=-DMB=$(MB)
endif

OBJS = acl.o arrayfuncs.o arrayutils.o bool.o cash.o char.o chunk.o date.o \
	datum.o dt.o filename.o float.o geo_ops.o geo_selfuncs.o int.o \
	misc.o nabstime.o name.o not_in.o numutils.o oid.o \
	oidname.o oidint2.o oidint4.o oracle_compat.o regexp.o regproc.o  \
	selfuncs.o \
	tid.o varchar.o varlena.o sets.o datetime.o like.o timestamp.o version.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

