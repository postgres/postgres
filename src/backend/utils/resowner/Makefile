#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/resowner
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/resowner/Makefile,v 1.1 2004/07/17 03:30:10 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/resowner
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = resowner.o

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
