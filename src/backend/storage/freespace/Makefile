#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/freespace
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/freespace/Makefile,v 1.1 2001/06/27 23:31:39 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/freespace
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = freespace.o

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
