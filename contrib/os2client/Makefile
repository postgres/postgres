#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for libpq library
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/contrib/os2client/Attic/Makefile,v 1.1 1998/10/31 04:10:52 scrappy Exp $
#
#-------------------------------------------------------------------------

SO_MAJOR_VERSION=1
SO_MINOR_VERSION=1

SRCDIR= ../../src

INTERFACES= $(SRCDIR)/interfaces/libpq

PORTNAME=OS2

CC=gcc
CFLAGS=-I. -I$(SRCDIR)/include
CFLAGS+=-DFRONTEND -DTCPIPV4 -DHAVE_CRYPT_H

CP= copy

AR=ar

AROPT=rc

RANLIB= ar s

LDFLAGS= -L.

OBJS= fe-auth.o fe-connect.o fe-exec.o fe-misc.o fe-lobj.o fe-print.o \
      dllist.o pqsignal.o pqcomprim.o

EXEOBJS= psql.o stringutils.o

all: libpq.a c.h psql

fe-auth.o: $(INTERFACES)/fe-auth.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/fe-auth.c

fe-connect.o: $(INTERFACES)/fe-connect.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/fe-connect.c

fe-exec.o: $(INTERFACES)/fe-exec.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/fe-exec.c

fe-lobj.o: $(INTERFACES)/fe-lobj.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/fe-lobj.c

fe-misc.o: $(INTERFACES)/fe-misc.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/fe-misc.c

fe-print.o: $(INTERFACES)/fe-print.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/fe-print.c

pqsignal.o: $(INTERFACES)/pqsignal.c
	$(CC) $(CFLAGS) -c $(INTERFACES)/pqsignal.c

dllist.o: $(SRCDIR)/backend/lib/dllist.c
	$(CC) $(CFLAGS) -c $(SRCDIR)/backend/lib/dllist.c

pqcomprim.o: $(SRCDIR)/backend/libpq/pqcomprim.c
	$(CC) $(CFLAGS) -c $(SRCDIR)/backend/libpq/pqcomprim.c

libpq.a: $(OBJS)
	$(AR) $(AROPT) libpq.a $(OBJS) 
	$(RANLIB) libpq.a

psql: $(EXEOBJS)
	$(CC) -o psql.exe $(EXEOBJS) $(LDFLAGS) -llibpq -lsocket -lufc

psql.o: $(SRCDIR)/bin/psql/psql.c
	$(CC) $(CFLAGS) -I$(INTERFACES) -c $(SRCDIR)/bin/psql/psql.c 

stringutils.o: $(SRCDIR)/bin/psql/stringutils.c
	$(CC) $(CFLAGS) -I$(INTERFACES) -c $(SRCDIR)/bin/psql/stringutils.c

c.h: $(SRCDIR)/include/c.h
	rm -f c.h
	$(CP) portname c.h
	cat $(SRCDIR)/include/c.h >> c.h

clean:
	rm -f libpq.a $(OBJS) $(EXEOBJS) psql.exe c.h dllist.c pqcomprim.c

