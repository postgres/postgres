#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/large_object
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/large_object/Makefile,v 1.12 2007/01/20 17:16:13 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/large_object
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = inv_api.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
