#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/common/Makefile,v 1.23 2007/01/20 17:16:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/common
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heaptuple.o indextuple.o printtup.o reloptions.o scankey.o tupdesc.o  

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
