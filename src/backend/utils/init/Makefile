#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/init
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/init/Makefile,v 1.1 1996/10/27 09:54:01 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
              -I../../include \
              -I../../../include

CFLAGS += $(INCLUDE_OPT)
# The following defines really ought to go in config.h 
CFLAGS += -DPOSTGRESDIR='"$(POSTGRESDIR)"' -DPGDATADIR='"$(DATADIR)"' \
          -DPOSTPORT='"$(POSTPORT)"'

OBJS = enbl.o findbe.o globals.o magic.o miscinit.o postinit.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

