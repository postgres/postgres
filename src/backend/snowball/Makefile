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

stop_files = \
	danish.stop \
	dutch.stop \
	english.stop \
	finnish.stop \
	french.stop \
	german.stop \
	hungarian.stop \
	italian.stop \
	nepali.stop \
	norwegian.stop \
	portuguese.stop \
	russian.stop \
	spanish.stop \
	swedish.stop \
	turkish.stop

SQLSCRIPT= snowball_create.sql
DICTDIR=tsearch_data

VPATH += $(srcdir)/libstemmer

NAME := dict_snowball
rpath =

all: all-shared-lib $(SQLSCRIPT)

include $(top_srcdir)/src/Makefile.shlib

$(SQLSCRIPT): snowball_create.pl snowball_func.sql.in snowball.sql.in
	$(PERL) $< --input ${srcdir} --outdir .

install: all installdirs install-lib install-script
	$(INSTALL_DATA) $(addprefix $(srcdir)/stopwords/,$(stop_files)) '$(DESTDIR)$(datadir)/$(DICTDIR)'

# $(SQLSCRIPT) might be in the srcdir or the build dir
install-script: $(SQLSCRIPT)
	$(INSTALL_DATA) $< '$(DESTDIR)$(datadir)'

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(datadir)/$(DICTDIR)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(datadir)/$(SQLSCRIPT)'
	rm -f $(addprefix '$(DESTDIR)$(datadir)/$(DICTDIR)/',$(stop_files))

clean distclean: clean-lib
	rm -f $(OBJS)
	rm -f $(SQLSCRIPT)
