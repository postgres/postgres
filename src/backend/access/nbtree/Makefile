#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/nbtree
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/nbtree/Makefile,v 1.14 2003/11/29 19:51:40 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/nbtree
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = nbtcompare.o nbtinsert.o nbtpage.o nbtree.o nbtsearch.o \
       nbtutils.o nbtsort.o nbtxlog.o

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

