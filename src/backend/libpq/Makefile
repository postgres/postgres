#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/libpq/Makefile,v 1.7 1997/04/04 10:39:19 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

# kerberos flags
ifdef KRBVERS
CFLAGS+= $(KRBFLAGS)
LDFLAGS+= $(KRBLIBS)
endif

OBJS = be-dumpdata.o be-fsstubs.o be-pqexec.o pqcomprim.o\
       auth.o hba.o pqcomm.o portal.o util.o portalbuf.o pqpacket.o pqsignal.o \
       password.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

be-dumpdata.o be-pqexec.o: ../fmgr.h

../fmgr.h: 
	$(MAKE) -C .. fmgr.h

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

