#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for catalog
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/catalog/Makefile,v 1.10 1998/04/06 00:22:13 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

OBJS = catalog.o heap.o index.o indexing.o aclchk.o \
       pg_aggregate.o pg_operator.o pg_proc.o pg_type.o

all: SUBSYS.o global1.bki.source local1_template1.bki.source \
		global1.description local1_template1.description

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

GENBKI= ./genbki.sh

GLOBALBKI_SRCS= $(addprefix ../../include/catalog/, \
                  pg_database.h pg_variable.h pg_shadow.h \
                  pg_group.h pg_log.h \
                 )

LOCALBKI_SRCS= $(addprefix ../../include/catalog/, \
                 pg_proc.h pg_type.h pg_attribute.h pg_class.h \
                 pg_inherits.h pg_index.h pg_version.h pg_statistic.h \
                 pg_operator.h pg_opclass.h pg_am.h pg_amop.h pg_amproc.h \
                 pg_language.h pg_parg.h \
                 pg_aggregate.h pg_ipl.h pg_inheritproc.h \
                 pg_rewrite.h pg_listener.h pg_description.h indexing.h \
                )

global1.bki.source: $(GENBKI) $(GLOBALBKI_SRCS)
	sh $(SHOPTS) $(GENBKI) $(BKIOPTS) $(GLOBALBKI_SRCS) > $@ 2>global1.description

local1_template1.bki.source: $(GENBKI) $(LOCALBKI_SRCS)
	sh $(SHOPTS) $(GENBKI) $(BKIOPTS) $(LOCALBKI_SRCS) > $@ 2>local1_template1.description

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) global1.bki.source local1_template1.bki.source \
				global1.description local1_template1.description

ifeq (depend,$(wildcard depend))
include depend
endif

