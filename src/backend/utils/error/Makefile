#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/error
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/utils/error/Makefile,v 1.11 2003/11/29 19:52:01 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/error
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = assert.o elog.o

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
