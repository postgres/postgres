#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/error
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/error/Makefile,v 1.9 2000/08/31 16:10:48 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/error
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = assert.o elog.o exc.o excabort.o excid.o format.o

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
