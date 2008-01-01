#-------------------------------------------------------------------------
#
# Makefile for backend/tsearch
#
# Copyright (c) 2006-2008, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/backend/tsearch/Makefile,v 1.4 2008/01/01 19:45:52 momjian Exp $
#
#-------------------------------------------------------------------------
subdir = src/backend/tsearch
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

DICTDIR=tsearch_data

DICTFILES=synonym_sample.syn thesaurus_sample.ths hunspell_sample.affix \
	ispell_sample.affix ispell_sample.dict

OBJS = ts_locale.o ts_parse.o wparser.o wparser_def.o dict.o \
	dict_simple.o dict_synonym.o dict_thesaurus.o \
	dict_ispell.o regis.o spell.o \
	to_tsany.o ts_utils.o

all: SUBSYS.o 

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $^

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

.PHONY: install-data
install-data: $(DICTFILES) installdirs
	for i in $(DICTFILES); \
		do $(INSTALL_DATA) $(srcdir)/$$i '$(DESTDIR)$(datadir)/$(DICTDIR)/'$$i; \
	done

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/$(DICTDIR)'

.PHONY: uninstall-data
uninstall-data:
	for i in $(DICTFILES); \
		do rm -rf '$(DESTDIR)$(datadir)/$(DICTDIR)/'$$i ; \
	done

clean distclean maintainer-clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
