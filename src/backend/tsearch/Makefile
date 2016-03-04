#-------------------------------------------------------------------------
#
# Makefile for backend/tsearch
#
# Copyright (c) 2006-2016, PostgreSQL Global Development Group
#
# src/backend/tsearch/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/tsearch
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

DICTDIR=tsearch_data

DICTFILES=dicts/synonym_sample.syn dicts/thesaurus_sample.ths \
	dicts/hunspell_sample.affix \
	dicts/ispell_sample.affix dicts/ispell_sample.dict \
	dicts/hunspell_sample_long.affix dicts/hunspell_sample_long.dict \
	dicts/hunspell_sample_num.affix dicts/hunspell_sample_num.dict

OBJS = ts_locale.o ts_parse.o wparser.o wparser_def.o dict.o \
	dict_simple.o dict_synonym.o dict_thesaurus.o \
	dict_ispell.o regis.o spell.o \
	to_tsany.o ts_selfuncs.o ts_typanalyze.o ts_utils.o

include $(top_srcdir)/src/backend/common.mk

.PHONY: install-data
install-data: $(DICTFILES) installdirs
	$(INSTALL_DATA) $(addprefix $(srcdir)/,$(DICTFILES)) '$(DESTDIR)$(datadir)/$(DICTDIR)/'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/$(DICTDIR)'

.PHONY: uninstall-data
uninstall-data:
	rm -rf $(addprefix '$(DESTDIR)$(datadir)/$(DICTDIR)/',$(DICTFILES))
