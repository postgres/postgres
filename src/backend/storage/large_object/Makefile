#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/large_object
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/storage/large_object/Makefile,v 1.11 2003/11/29 19:51:56 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/large_object
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = inv_api.o

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
