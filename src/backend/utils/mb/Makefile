#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/Makefile,v 1.8 2000/05/29 05:45:34 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

OBJS = common.o conv.o mbutils.o wchar.o wstrcmp.o wstrncmp.o variable.o \
       big5.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

sjistest: $(OBJS)
	$(CC) -c $(CFLAGS) -DDEBUGMAIN conv.c
	$(CC) -o sjistest conv.o \
	common.o mbutils.o wchar.o wstrcmp.o wstrncmp.o variable.o \
       big5.o $(LDFLAGS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

