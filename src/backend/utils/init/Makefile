#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/init
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/init/Makefile,v 1.2 1996/11/09 06:23:41 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

INCLUDE_OPT = -I../.. \
              -I../../port/$(PORTNAME) \
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

