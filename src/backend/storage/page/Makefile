#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/page
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/page/Makefile,v 1.10 2000/08/31 16:10:37 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/page
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS =  bufpage.o itemptr.o

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
