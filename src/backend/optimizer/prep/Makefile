#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/prep
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/optimizer/prep/Makefile,v 1.15 2003/11/29 19:51:51 pgsql Exp $
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

