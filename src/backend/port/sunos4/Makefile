#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for port/sunos4
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/port/sunos4/Makefile,v 1.8 2003/11/29 19:51:54 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port/sunos4
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = strtol.o

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
