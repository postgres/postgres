#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mmgr
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/mmgr/Makefile,v 1.11 2003/11/29 19:52:04 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = aset.o mcxt.o portalmem.o

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
