#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/nbtree
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/nbtree/Makefile,v 1.15 2007/01/20 17:16:10 petere Exp $
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

clean: 
	rm -f SUBSYS.o $(OBJS)
