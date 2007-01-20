#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/index
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/index/Makefile,v 1.13 2007/01/20 17:16:10 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/index
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = genam.o indexam.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
