#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for lib (miscellaneous stuff)
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/lib/Makefile,v 1.19 2004/04/25 18:23:56 neilc Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/lib
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = dllist.o stringinfo.o

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
