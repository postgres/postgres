#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/libpq/Makefile,v 1.19 2000/05/29 05:44:46 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

# kerberos flags
ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
LDFLAGS+= $(KRBLIBS)
endif

OBJS = be-dumpdata.o be-fsstubs.o be-pqexec.o portal.o portalbuf.o \
	auth.o hba.o crypt.o password.o \
	pqcomm.o pqformat.o pqpacket.o pqsignal.o util.o


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

