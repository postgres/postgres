#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/freespace
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/freespace/Makefile,v 1.2 2003/11/29 19:51:56 pgsql Exp $
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
