#-------------------------------------------------------------------------
#
# Makefile for src/backend/snowball
#
# $PostgreSQL: pgsql/src/backend/snowball/Makefile,v 1.4 2007/10/23 20:46:12 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/snowball
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(top_srcdir)/src/include/snowball \
	-I$(top_srcdir)/src/include/snowball/libstemmer $(CPPFLAGS)

OBJS= dict_snowball.o api.o utilities.o \
	stem_ISO_8859_1_danish.o \
	stem_ISO_8859_1_dutch.o \
	stem_ISO_8859_1_english.o \
	stem_ISO_8859_1_finnish.o \
	stem_ISO_8859_1_french.o \
	stem_ISO_8859_1_german.o \
	stem_ISO_8859_1_hungarian.o \
	stem_ISO_8859_1_italian.o \
	stem_ISO_8859_1_norwegian.o \
	stem_ISO_8859_1_porter.o \
	stem_ISO_8859_1_portuguese.o \
	stem_ISO_8859_1_spanish.o \
	stem_ISO_8859_1_swedish.o \
	stem_ISO_8859_2_romanian.o \
	stem_KOI8_R_russian.o \
	stem_UTF_8_danish.o \
	stem_UTF_8_dutch.o \
	stem_UTF_8_english.o \
	stem_UTF_8_finnish.o \
	stem_UTF_8_french.o \
	stem_UTF_8_german.o \
	stem_UTF_8_hungarian.o \
	stem_UTF_8_italian.o \
	stem_UTF_8_norwegian.o \
	stem_UTF_8_porter.o \
	stem_UTF_8_portuguese.o \
	stem_UTF_8_romanian.o \
	stem_UTF_8_russian.o \
	stem_UTF_8_spanish.o \
	stem_UTF_8_swedish.o \
	stem_UTF_8_turkish.o

# first column is language name and also name of dictionary for not-all-ASCII
# words, second is name of dictionary for all-ASCII words
# Note order dependency: use of some other language as ASCII dictionary
# must come after creation of that language
LANGUAGES=  \
	danish		danish 		\
	dutch 		dutch 		\
	english 	english		\
	finnish 	finnish		\
	french 		french		\
	german 		german		\
	hungarian 	hungarian	\
	italian 	italian		\
	norwegian 	norwegian	\
	portuguese 	portuguese	\
	romanian 	romanian	\
	russian 	english		\
	spanish 	spanish		\
	swedish 	swedish		\
	turkish		turkish


SQLSCRIPT= snowball_create.sql
DICTDIR=tsearch_data

ifdef VPATH
override VPATH := $(srcdir)/libstemmer:$(VPATH)
else
VPATH = $(srcdir)/libstemmer
endif

SHLIB_LINK 	:= $(BE_DLLLIBS)

NAME := dict_snowball
SO_MAJOR_VERSION := 0
SO_MINOR_VERSION := 0
rpath =

all: all-shared-lib $(SQLSCRIPT)

include $(top_srcdir)/src/Makefile.shlib

$(SQLSCRIPT): Makefile snowball_func.sql.in snowball.sql.in
ifeq ($(enable_shared), yes)
	echo '-- Language-specific snowball dictionaries' > $@
	cat $(srcdir)/snowball_func.sql.in >> $@	
	@set $(LANGUAGES) ; \
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
else
	echo "-- No language-specific snowball dictionaries, for lack of shared library support" > $@
endif

install: all installdirs
ifeq ($(enable_shared), yes)
	$(INSTALL_SHLIB) $(shlib) '$(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)'
endif
	$(INSTALL_DATA) $(SQLSCRIPT) '$(DESTDIR)$(datadir)'
	@set $(LANGUAGES) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		lang=$$1; shift; shift; \
		if [ -s $(srcdir)/stopwords/$${lang}.stop ] ; then \
			$(INSTALL_DATA) $(srcdir)/stopwords/$${lang}.stop '$(DESTDIR)$(datadir)/$(DICTDIR)' ; \
		fi \
	done

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(pkglibdir)' '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/$(DICTDIR)'

uninstall:
	rm -f '$(DESTDIR)$(pkglibdir)/$(NAME)$(DLSUFFIX)'
	rm -f '$(DESTDIR)$(datadir)/$(SQLSCRIPT)'
	@set $(LANGUAGES) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		lang=$$1; shift; shift;  \
		if [ -s $(srcdir)/stopwords/$${lang}.stop ] ; then \
		    rm -f '$(DESTDIR)$(datadir)/$(DICTDIR)/'$${lang}.stop ; \
		fi \
	done

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(SQLSCRIPT)
