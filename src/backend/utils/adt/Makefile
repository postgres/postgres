#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/adt
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/adt/Makefile,v 1.29 1999/12/13 22:34:33 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

CFLAGS += -I../..

# seems to be required for some date/time stuff 1999/07/22 bjm
ifeq ($(CPU),alpha)
ifeq ($(CC), gcc)
CFLAGS+= -mieee
endif
ifeq ($(CC), egcs)
CFLAGS+= -mieee
endif
endif

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = acl.o arrayfuncs.o arrayutils.o bool.o cash.o char.o chunk.o \
	date.o datetime.o datum.o dt.o filename.o float.o \
	geo_ops.o geo_selfuncs.o int.o int8.o like.o \
	misc.o nabstime.o name.o not_in.o numeric.o numutils.o \
	oid.o oracle_compat.o \
	regexp.o regproc.o ruleutils.o selfuncs.o sets.o \
	tid.o timestamp.o varchar.o varlena.o version.o \
	network.o mac.o inet_net_ntop.o inet_net_pton.o \
	ri_triggers.o pg_lzcompress.o lztext.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
