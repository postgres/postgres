#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/libpq/Makefile,v 1.23 2000/07/09 13:48:45 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/libpq
top_builddir = ../../..
include ../../Makefile.global

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = be-fsstubs.o \
	auth.o crypt.o hba.o password.o \
	pqcomm.o pqformat.o pqpacket.o pqsignal.o util.o

# This location might depend on the installation directories. Therefore
# we can't subsitute it into config.h.
ifdef krb_srvtab
CPPFLAGS += -DPG_KRB_SRVTAB='"$(krb_srvtab)"'
endif


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

