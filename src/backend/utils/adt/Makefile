#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/adt
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/adt/Makefile,v 1.15 1998/07/26 04:30:52 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = acl.o arrayfuncs.o arrayutils.o bool.o cash.o char.o chunk.o \
	date.o datetime.o datum.o dt.o filename.o float.o \
	geo_ops.o geo_selfuncs.o int.o int8.o like.o \
	misc.o nabstime.o name.o not_in.o numutils.o \
	oid.o oidname.o oidint2.o oidint4.o oracle_compat.o \
	regexp.o regproc.o selfuncs.o sets.o tid.o timestamp.o \
	varchar.o varlena.o version.o

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

