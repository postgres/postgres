#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/prep
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/optimizer/prep/Makefile,v 1.14 2003/01/20 18:54:54 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/prep
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = prepjointree.o prepqual.o preptlist.o prepunion.o

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

