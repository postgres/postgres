#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/index
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/index/Makefile,v 1.11 2003/11/09 21:30:35 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/index
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = genam.o indexam.o

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
