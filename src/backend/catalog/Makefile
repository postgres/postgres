#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for catalog
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/catalog/Makefile,v 1.1 1996/10/27 09:47:07 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../include \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = catalog.o heap.o index.o indexing.o \
       pg_aggregate.o pg_operator.o pg_proc.o pg_type.o

all: SUBSYS.o global1.bki.source local1_template1.bki.source

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

GENBKI= ./genbki.sh

GLOBALBKI_SRCS= $(addprefix ../../include/catalog/, \
                  pg_database.h pg_demon.h pg_magic.h pg_defaults.h \
                  pg_variable.h pg_server.h pg_user.h pg_hosts.h \
                  pg_group.h pg_log.h pg_time.h \
                 )

LOCALBKI_SRCS= $(addprefix ../../include/catalog/, \
                 pg_proc.h pg_type.h pg_attribute.h pg_class.h \
                 pg_inherits.h pg_index.h pg_version.h pg_statistic.h \
                 pg_operator.h pg_opclass.h pg_am.h pg_amop.h pg_amproc.h \
                 pg_language.h pg_parg.h \
                 pg_aggregate.h pg_ipl.h pg_inheritproc.h \
                 pg_rewrite.h pg_listener.h indexing.h \
                )

global1.bki.source: $(GENBKI) $(GLOBALBKI_SRCS)
	sh $(SHOPTS) $(GENBKI) $(BKIOPTS) $(GLOBALBKI_SRCS) > $@

local1_template1.bki.source: $(GENBKI) $(LOCALBKI_SRCS)
	sh $(SHOPTS) $(GENBKI) $(BKIOPTS) $(LOCALBKI_SRCS) > $@

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) global1.bki.source local1_template1.bki.source


ifeq (depend,$(wildcard depend))
include depend
endif

