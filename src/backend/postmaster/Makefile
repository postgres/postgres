#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for postmaster
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/postmaster/Makefile,v 1.13 2001/06/22 19:16:22 wieck Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = postmaster.o pgstat.o

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
