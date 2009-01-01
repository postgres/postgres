#-------------------------------------------------------------------------
#
# Makefile for backend/tsearch
#
# Copyright (c) 2006-2009, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/backend/tsearch/Makefile,v 1.9 2009/01/01 17:23:48 momjian Exp $
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
	to_tsany.o ts_selfuncs.o ts_typanalyze.o ts_utils.o

include $(top_srcdir)/src/backend/common.mk

.PHONY: install-data
install-data: $(DICTFILES) installdirs
	for i in $(DICTFILES); \
		do $(INSTALL_DATA) $(srcdir)/$$i '$(DESTDIR)$(datadir)/$(DICTDIR)/'$$i || exit; \
	done

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/$(DICTDIR)'

.PHONY: uninstall-data
uninstall-data:
	for i in $(DICTFILES); \
		do rm -rf '$(DESTDIR)$(datadir)/$(DICTDIR)/'$$i || exit; \
	done
