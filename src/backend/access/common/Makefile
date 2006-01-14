#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/common/Makefile,v 1.21 2006/01/14 22:03:35 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/common
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heaptuple.o indextuple.o printtup.o scankey.o tupdesc.o  

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

dep depend:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
