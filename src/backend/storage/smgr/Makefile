#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/smgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/smgr/Makefile,v 1.15 2004/02/10 01:55:26 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/smgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = md.o smgr.o smgrtype.o

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
