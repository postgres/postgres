#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the system catalogs module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/catalog/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:14 scrappy Exp $
#
#-------------------------------------------------------------------------

catdir=$(CURDIR)/catalog
VPATH:=$(VPATH):$(catdir)


SRCS_CATALOG= catalog.c heap.c index.c indexing.c \
	pg_aggregate.c pg_operator.c pg_proc.c pg_type.c

HEADERS+= catalog.h catname.h heap.h index.h indexing.h pg_aggregate.h \
	pg_am.h pg_amop.h pg_amproc.h pg_attribute.h pg_database.h \
	pg_defaults.h pg_demon.h pg_group.h pg_index.h pg_inheritproc.h \
	pg_inherits.h pg_ipl.h pg_language.h pg_listener.h \
	pg_log.h pg_magic.h pg_opclass.h pg_operator.h pg_parg.h \
	pg_proc.h pg_class.h \
	pg_rewrite.h pg_server.h pg_statistic.h pg_time.h pg_type.h \
	pg_user.h pg_variable.h pg_version.h

#
# The following is to create the .bki files.
# TODO: sort headers, (figure some automatic way of of determining
#  the bki sources?)
#
# XXX - more grot.  includes names and uid's in the header file.  FIX THIS
#  (not sure if i got this right - which do i need - or should i 
#   burn the whole damned thing)
#
ifdef ALLOW_PG_GROUP
BKIOPTS= -DALLOW_PG_GROUP
endif

GENBKI= $(catdir)/genbki.sh
BKIFILES= global1.bki local1_template1.bki 

GLOBALBKI_SRCS= pg_database.h pg_demon.h pg_magic.h pg_defaults.h \
	pg_variable.h pg_server.h pg_user.h pg_hosts.h \
	pg_group.h pg_log.h pg_time.h

LOCALBKI_SRCS= pg_proc.h pg_type.h pg_attribute.h pg_class.h \
	pg_inherits.h pg_index.h pg_version.h pg_statistic.h pg_operator.h \
	pg_opclass.h pg_am.h pg_amop.h pg_amproc.h pg_language.h pg_parg.h \
	pg_aggregate.h pg_ipl.h pg_inheritproc.h \
	pg_rewrite.h pg_listener.h indexing.h

global1.bki: $(GENBKI) $(GLOBALBKI_SRCS)
	sh $(SHOPTS) $(GENBKI) $(BKIOPTS) \
		$(patsubst $(GENBKI),,$^) > $(objdir)/$(@F)


local1_template1.bki: $(GENBKI) $(LOCALBKI_SRCS)
	sh $(SHOPTS) $(GENBKI) $(BKIOPTS) \
		$(patsubst $(GENBKI),,$^) > $(objdir)/$(@F)


#${PROG}: ${BKIFILES}
#

CLEANFILES+= ${BKIFILES}
