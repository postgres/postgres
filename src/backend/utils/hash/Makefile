#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/hash
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/hash/Makefile,v 1.10 2001/03/13 01:17:06 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/hash
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = dynahash.o hashfn.o pg_crc.o

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
