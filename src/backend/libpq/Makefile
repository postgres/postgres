#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/libpq/Makefile,v 1.13 1998/07/26 04:30:27 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

# kerberos flags
ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
LDFLAGS+= $(KRBLIBS)
endif

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = be-dumpdata.o be-fsstubs.o be-pqexec.o pqcomprim.o\
       auth.o hba.o crypt.o pqcomm.o portal.o util.o portalbuf.o pqpacket.o pqsignal.o \
       password.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

be-dumpdata.o be-pqexec.o: ../fmgr.h

../fmgr.h: 
	$(MAKE) -C .. fmgr.h

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

