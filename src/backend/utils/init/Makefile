#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/init
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/init/Makefile,v 1.17 2004/05/11 21:57:14 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/init
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = globals.o miscinit.o postinit.o

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
