#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gist
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/gist/Makefile,v 1.13 2005/06/14 11:45:13 teodor Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gist
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = gist.o gistutil.o gistxlog.o gistget.o gistscan.o

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
