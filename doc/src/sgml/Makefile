#----------------------------------------------------------------------------
#
# Makefile
#	Postgres documentation makefile
#	Thomas Lockhart
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/doc/src/sgml/Makefile,v 1.31 2001/02/13 21:27:05 petere Exp $
#
#----------------------------------------------------------------------------

subdir = doc/src/sgml
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

.SECONDARY:
.NOTPARALLEL:

ifndef DOCBOOKSTYLE
DOCBOOKSTYLE = /home/projects/pgsql/developers/thomas/db143.d/docbook
endif

ifndef JADE
JADE = jade
endif
SGMLINCLUDE = -D ref -D ../graphics

ifndef NSGMLS
NSGMLS = nsgmls
endif

ifndef SGMLSPL
SGMLSPL = sgmlspl
endif


# docbook2man generates man pages from docbook refentry source code.

ifndef D2MDIR
# This is where the (patched) docbook2man perl scripts reside on hub.org.
D2MDIR= /home/users/t/thomas/d2x/docbook2man
endif
D2MSCRIPT= $(D2MDIR)/docbook2man-spec.pl

vpath %.sgml ./ref

ALLBOOKS = admin developer programmer reference tutorial user

ALLSGML := $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml)


catalog: Makefile $(top_builddir)/src/Makefile.global $(top_builddir)/src/Makefile.custom
	(echo "PUBLIC \"-//Norman Walsh//DOCUMENT DocBook HTML Stylesheet//EN\" \"$(DOCBOOKSTYLE)/html/docbook.dsl\""; \
	echo "PUBLIC \"-//Norman Walsh//DOCUMENT DocBook Print Stylesheet//EN\" \"$(DOCBOOKSTYLE)/print/docbook.dsl\"") > $@

ifeq "$(origin DOCBOOKSTYLE)" "command line"
.PHONY: catalog
endif


##
## Man pages
##

.PHONY: man

DEFAULTSECTION := l

man: $(ALLSGML)
	$(NSGMLS) $(NSGMLS_FLAGS) $(srcdir)/book-decl.sgml $(srcdir)/reference.sgml \
	  | $(SGMLSPL) $(D2MSCRIPT) --lowercase --section $(DEFAULTSECTION)
# One more time, to resolve cross-references
	$(NSGMLS) $(NSGMLS_FLAGS) $(srcdir)/book-decl.sgml $(srcdir)/reference.sgml \
	  | $(SGMLSPL) $(D2MSCRIPT) --lowercase --section $(DEFAULTSECTION)
	$(mkinstalldirs) man1 man$(DEFAULTSECTION)
	mv *.1 man1/
	mv *.$(DEFAULTSECTION) man$(DEFAULTSECTION)/


##
## HTML
##

JADE.html = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -d stylesheet.dsl -i output-html -t sgml

postgres.html: postgres.sgml $(ALLSGML) stylesheet.dsl catalog
	@rm -f *.html
	$(JADE.html) $<
	ln -sf $@ index.html

$(addsuffix .html, $(ALLBOOKS)): %.html: %.sgml $(ALLSGML) stylesheet.dsl catalog
	@rm -f *.html
	$(JADE.html) book-decl.sgml $<
	ln -sf $@ index.html


##
## Print
##

# RTF to allow minor editing for hardcopy
$(addsuffix .rtf, $(ALLBOOKS)): %.rtf: %.sgml $(ALLSGML) stylesheet.dsl catalog
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -d stylesheet.dsl -t rtf -V rtf-backend -i output-print book-decl.sgml $<

# TeX and DVI
$(addsuffix .tex, $(ALLBOOKS)): %.tex: %.sgml $(ALLSGML) stylesheet.dsl catalog
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -d stylesheet.dsl -t tex -V tex-backend -i output-print book-decl.sgml $<

%.dvi: %.tex
	jadetex $<
	jadetex $<
	jadetex $<

# Postscript from TeX
%.ps: %.dvi
	dvips -o $@ $<

%.pdf: %.tex
	pdfjadetex $<
	pdfjadetex $<
	pdfjadetex $<


# Graphics

%.gif:
	cp -p ../graphics/$@ .


##
## Semi-automatic generation of some text files.
##

INSTALL HISTORY: % : %.html
	@echo "|";\
	 echo "| You should now take \`$<', save it as a text file in Netscape,";\
	 echo "| and put it in place of the existing \`$@' file.";\
	 echo "|"


INSTALL.html HISTORY.html: %.html : tempfile_%.html
	sed 's/Chapter 1. *//g' $< >$@

tempfile_INSTALL.html tempfile_HISTORY.html: tempfile_%.html : tempfile_%.sgml
	$(JADE) $(JADEFLAGS) -d $(HDSL) -V nochunks -t sgml $< >$@


tempfile_INSTALL.sgml: standalone-install.sgml installation.sgml
	cat $+ >$@

tempfile_HISTORY.sgml: release.sgml
	( echo '<!doctype chapter PUBLIC "-//OASIS//DTD DocBook V3.1//EN">'; \
	  cat $< ) >$@


.INTERMEDIATE: tempfile_INSTALL.html tempfile_HISTORY.html tempfile_INSTALL.sgml tempfile_HISTORY.sgml


##
## Check
##

check: $(addprefix check-, $(ALLBOOKS)) check-postgres

# Quick syntax check without style processing
$(addprefix check-, $(ALLBOOKS)): check-%: %.sgml
	$(NSGMLS) $(SGMLINCLUDE) -s book-decl.sgml $<

check-postgres: postgres.sgml
	$(NSGMLS) $(SGMLINCLUDE) -s $<


##
## Clean
##

clean distclean maintainer-clean:
	rm -f catalog
# HTML
	rm -f HTML.manifest *.html
# man
	rm -rf *.1 *.$(DEFAULTSECTION) man1 man$(DEFAULTSECTION) manpage.refs manpage.links manpage.log
# print
	rm -f *.rtf *.tex *.dvi *.aux *.log *.ps *.pdf
