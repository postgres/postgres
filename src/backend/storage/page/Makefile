#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/page
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/page/Makefile,v 1.12 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/page
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS =  bufpage.o itemptr.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
