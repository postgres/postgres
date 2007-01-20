#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/freespace
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/freespace/Makefile,v 1.3 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/freespace
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = freespace.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
