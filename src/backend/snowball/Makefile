#-------------------------------------------------------------------------
#
# Makefile for src/backend/snowball
#
# src/backend/snowball/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "snowball - natural language stemmers"
PGAPPICON = win32

subdir = src/backend/snowball
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_srcdir)/src/include/snowball \
	-I$(top_srcdir)/src/include/snowball/libstemmer $(CPPFLAGS)

OBJS = \
	$(WIN32RES) \
	api.o \
	dict_snowball.o \
	utilities.o

OBJS += \
	stem_ISO_8859_1_basque.o \
	stem_ISO_8859_1_catalan.o \
	stem_ISO_8859_1_danish.o \
	stem_ISO_8859_1_dutch.o \
	stem_ISO_8859_1_english.o \
	stem_ISO_8859_1_finnish.o \
	stem_ISO_8859_1_french.o \
	stem_ISO_8859_1_german.o \
	stem_ISO_8859_1_indonesian.o \
	stem_ISO_8859_1_irish.o \
	stem_ISO_8859_1_italian.o \
	stem_ISO_8859_1_norwegian.o \
	stem_ISO_8859_1_porter.o \
	stem_ISO_8859_1_portuguese.o \
	stem_ISO_8859_1_spanish.o \
	stem_ISO_8859_1_swedish.o \
	stem_ISO_8859_2_hungarian.o \
	stem_ISO_8859_2_romanian.o \
	stem_KOI8_R_russian.o \
	stem_UTF_8_arabic.o \
	stem_UTF_8_armenian.o \
	stem_UTF_8_basque.o \
	stem_UTF_8_catalan.o \
	stem_UTF_8_danish.o \
	stem_UTF_8_dutch.o \
	stem_UTF_8_english.o \
	stem_UTF_8_finnish.o \
	stem_UTF_8_french.o \
	stem_UTF_8_german.o \
	stem_UTF_8_greek.o \
	stem_UTF_8_hindi.o \
	stem_UTF_8_hungarian.o \
	stem_UTF_8_indonesian.o \
	stem_UTF_8_irish.o \
	stem_UTF_8_italian.o \
	stem_UTF_8_lithuanian.o \
	stem_UTF_8_nepali.o \
	stem_UTF_8_norwegian.o \
	stem_UTF_8_porter.o \
	stem_UTF_8_portuguese.o \
	stem_UTF_8_romanian.o \
	stem_UTF_8_russian.o \
	stem_UTF_8_serbian.o \
	stem_UTF_8_spanish.o \
	stem_UTF_8_swedish.o \
	stem_UTF_8_tamil.o \
	stem_UTF_8_turkish.o \
	stem_UTF_8_yiddish.o

# first column is language name and also name of dictionary for not-all-ASCII
# words, second is name of dictionary for all-ASCII words
# Note order dependency: use of some other language as ASCII dictionary
# must come after creation of that language
LANGUAGES=  \
	arabic		arabic		\
	armenian	armenian	\
	basque		basque		\
	catalan		catalan		\
	danish		danish		\
	dutch		dutch		\
	english		english		\
	finnish		finnish		\
	french		french		\
	german		german		\
	greek		greek		\
	hindi		english		\
	hungarian	hungarian	\
	indonesian	indonesian	\
	irish		irish		\
	italian		italian		\
	lithuanian	lithuanian	\
	nepali		nepali		\
	norwegian	norwegian	\
	portuguese	portuguese	\
	romanian	romanian	\
	russian		english		\
	serbian		serbian		\
	spanish		spanish		\
	swedish		swedish		\
	tamil		tamil		\
	turkish		turkish		\
	yiddish		yiddish


SQLSCRIPT= snowball_create.sql
DICTDIR=tsearch_data

VPATH += $(srcdir)/libstemmer

NAME := dict_snowball
rpath =

all: all-shared-lib $(SQLSCRIPT)

include $(top_srcdir)/src/Makefile.shlib

$(SQLSCRIPT): Makefile snowball_func.sql.in snowball.sql.in
	echo '-- Language-specific snowball dictionaries' > $@
	cat $(srcdir)/snowball_func.sql.in >> $@
	@set -e; \
	set $(LANGUAGES) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		lang=$$1; shift; \
		nonascdictname=$$lang; \
		ascdictname=$$1; shift; \
		if [ -s $(srcdir)/stopwords/$${lang}.stop ] ; then \
			stop=", StopWords=$${lang}" ; \
		else \
			stop=""; \
		fi; \
		cat $(srcdir)/snowball.sql.in | \
			sed -e "s#_LANGNAME_#$$lang#g" | \
			sed -e "s#_DICTNAME_#$${lang}_stem#g" | \
			sed -e "s#_CFGNAME_#$$lang#g" | \
			sed -e "s#_ASCDICTNAME_#$${ascdictname}_stem#g" | \
			sed -e "s#_NONASCDICTNAME_#$${nonascdictname}_stem#g" | \
			sed -e "s#_STOPWORDS_#$$stop#g" ; \
	done >> $@

install: all installdirs install-lib
	$(INSTALL_DATA) $(SQLSCRIPT) '$(DESTDIR)$(datadir)'
	@set -e; \
	set $(LANGUAGES) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		lang=$$1; shift; shift; \
		if [ -s $(srcdir)/stopwords/$${lang}.stop ] ; then \
			$(INSTALL_DATA) $(srcdir)/stopwords/$${lang}.stop '$(DESTDIR)$(datadir)/$(DICTDIR)' ; \
		fi \
	done

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/$(DICTDIR)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(datadir)/$(SQLSCRIPT)'
	@set -e; \
	set $(LANGUAGES) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		lang=$$1; shift; shift;  \
		if [ -s $(srcdir)/stopwords/$${lang}.stop ] ; then \
		    rm -f '$(DESTDIR)$(datadir)/$(DICTDIR)/'$${lang}.stop ; \
		fi \
	done

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(SQLSCRIPT)
