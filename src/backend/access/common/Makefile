#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/common/Makefile,v 1.22 2006/07/03 22:45:36 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/common
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heaptuple.o indextuple.o printtup.o reloptions.o scankey.o tupdesc.o  

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
