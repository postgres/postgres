#----------------------------------------------------------------------------
#
# PostgreSQL documentation makefile
#
# $PostgreSQL: pgsql/doc/src/sgml/Makefile,v 1.120 2009/08/04 22:04:37 petere Exp $
#
#----------------------------------------------------------------------------

subdir = doc/src/sgml
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

.NOTPARALLEL:
.PRECIOUS: %-A4.tex-ps %-US.tex-ps %-A4.tex-pdf %-US.tex-pdf

ifndef COLLATEINDEX
COLLATEINDEX = $(DOCBOOKSTYLE)/bin/collateindex.pl
endif

ifndef JADE
JADE = jade
endif
SGMLINCLUDE = -D $(srcdir)

# If this is a vpath build, some generated SGML will be in the build
# tree, so we need to make sure we look there as well as in the
# source tree
ifeq ($(vpath_build), yes)
SGMLINCLUDE += -D .
endif

ifndef NSGMLS
NSGMLS = nsgmls
endif

ifndef OSX
OSX = osx
endif

ifndef XSLTPROC
XSLTPROC = xsltproc
endif

override XSLTPROCFLAGS += --stringparam pg.version '$(VERSION)'


GENERATED_SGML = bookindex.sgml version.sgml \
	features-supported.sgml features-unsupported.sgml

ALLSGML := $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml) $(GENERATED_SGML)

# Sometimes we don't want this one.
ALMOSTALLSGML := $(filter-out %bookindex.sgml,$(ALLSGML))

ifdef DOCBOOKSTYLE
CATALOG = -c $(DOCBOOKSTYLE)/catalog
endif

# Enable some extra warnings
# -wfully-tagged needed to throw a warning on missing tags 
# for older tool chains, 2007-08-31
# Note: try "make SPFLAGS=-wxml" to catch a lot of other dubious constructs,
# in particular < and & that haven't been made into entities.  It's far too
# noisy to turn on by default, unfortunately.
override SPFLAGS += -wall -wno-unused-param -wno-empty -wfully-tagged

##
## Man pages
##

.PHONY: man

man: man-stamp

man-stamp: stylesheet-man.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) $^
	rm man1/SPI* man1/dblink*
	touch $@


##
## HTML
##

all: html

.PHONY: html draft

JADE.html.call = $(JADE) $(JADEFLAGS) $(SPFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t sgml -i output-html

# The draft target creates HTML output in draft mode, without index (for faster build).
draft: postgres.sgml $(ALMOSTALLSGML) stylesheet.dsl
	$(JADE.html.call) -V draft-mode $<

html: html-stamp

html-stamp: postgres.sgml $(ALLSGML) stylesheet.dsl
	@rm -f *.html
	$(JADE.html.call) -i include-index $<
ifeq ($(vpath_build), yes)
	@cp $(srcdir)/stylesheet.css .
endif
	touch $@

HTML.index: postgres.sgml $(ALMOSTALLSGML) stylesheet.dsl
	$(JADE.html.call) -V html-index $<

bookindex.sgml: HTML.index
	LC_ALL=C $(PERL) $(COLLATEINDEX) -f -g -i 'bookindex' -o $@ $<

version.sgml: $(top_builddir)/src/Makefile.global
	{ \
	  echo "<!entity version \"$(VERSION)\">"; \
	  echo "<!entity majorversion \"$(MAJORVERSION)\">"; \
	} >$@

features-supported.sgml: $(top_srcdir)/src/backend/catalog/sql_feature_packages.txt $(top_srcdir)/src/backend/catalog/sql_features.txt
	$(PERL) $(srcdir)/mk_feature_tables.pl YES $^ > $@

features-unsupported.sgml: $(top_srcdir)/src/backend/catalog/sql_feature_packages.txt $(top_srcdir)/src/backend/catalog/sql_features.txt
	$(PERL) $(srcdir)/mk_feature_tables.pl NO $^ > $@


##
## Print
##


# RTF to allow minor editing for hardcopy
%.rtf: %.sgml $(ALLSGML) html
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t rtf -V rtf-backend -i output-print  -i include-index postgres.sgml

# TeX
# Regular TeX and pdfTeX have slightly differing requirements, so we
# need to distinguish the path we're taking.

JADE.tex.call = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d $(srcdir)/stylesheet.dsl -t tex -V tex-backend -i output-print -i include-index

%-A4.tex-ps: %.sgml $(ALLSGML) html
	$(JADE.tex.call) -V texdvi-output -V '%paper-type%'=A4 -o $@ $<

%-US.tex-ps: %.sgml $(ALLSGML) html
	$(JADE.tex.call) -V texdvi-output -V '%paper-type%'=USletter -o $@ $<

%-A4.tex-pdf: %.sgml $(ALLSGML) html
	$(JADE.tex.call) -V texpdf-output -V '%paper-type%'=A4 -o $@ $<

%-US.tex-pdf: %.sgml $(ALLSGML) html
	$(JADE.tex.call) -V texpdf-output -V '%paper-type%'=USletter -o $@ $<

%.dvi: %.tex-ps
	@rm -f $*.aux $*.log
# multiple runs are necessary to create proper intra-document links
	jadetex $<
	jadetex $<
	jadetex $<

# PostScript from TeX
postgres.ps:
	$(error Invalid target;  use postgres-A4.ps or postgres-US.ps as targets)

%.ps: %.dvi
	dvips -o $@ $<

postgres.pdf:
	$(error Invalid target;  use postgres-A4.pdf or postgres-US.pdf as targets)

%.pdf: %.tex-pdf
	@rm -f $*.aux $*.log $*.out
# multiple runs are necessary to create proper intra-document links
	pdfjadetex $<
	pdfjadetex $<
	pdfjadetex $<


# This generates an XML version of the flow-object tree.  It's useful
# for debugging DSSSL code, and possibly to interface to some other
# tools that can make use of this.
%.fot: %.sgml $(ALLSGML) html
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t fot -i output-print -i include-index -o $@ $<


##
## Semi-automatic generation of some text files.
##

JADE.text = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -i output-text -t sgml
LYNX = lynx

INSTALL HISTORY regress_README: % : %.html
	$(PERL) -p -e 's/<H(1|2)$$/<H\1 align=center/g' $< | $(LYNX) -force_html -dump -nolist -stdin >$@

INSTALL.html: standalone-install.sgml installation.sgml version.sgml
	$(JADE.text) -V nochunks standalone-install.sgml installation.sgml >$@

HISTORY.html: generate_history.pl $(wildcard $(srcdir)/release*.sgml)
	$(PERL) $< "$(srcdir)" release.sgml >tempfile_HISTORY.sgml
	$(JADE.text) -V nochunks tempfile_HISTORY.sgml >$@
	rm tempfile_HISTORY.sgml

regress_README.html: regress.sgml
	( echo '<!doctype chapter PUBLIC "-//OASIS//DTD DocBook V4.2//EN" ['; \
	  echo '<!entity % standalone-ignore "IGNORE">'; \
	  echo '<!entity % standalone-include "INCLUDE"> ]>'; \
	  cat $< ) >tempfile_regress_README.sgml
	$(JADE.text) -V nochunks tempfile_regress_README.sgml >$@
	rm tempfile_regress_README.sgml


##
## XSLT processing
##

postgres.xml: postgres.sgml $(ALMOSTALLSGML)
	$(OSX) -D. -x lower $< | \
	  $(PERL) -p -e 's/\[(amp|copy|egrave|gt|lt|mdash|nbsp|ouml|pi|quot|uuml) *\]/\&\1;/g;' \
	             -e '$$_ .= qq{<!DOCTYPE book PUBLIC "-//OASIS//DTD DocBook XML V4.2//EN" "http://www.oasis-open.org/docbook/xml/4.2/docbookx.dtd">\n} if $$. == 1;' \
	  >$@
# ' hello Emacs

xslthtml: stylesheet.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) $^

htmlhelp: stylesheet-hh.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) $^

%-A4.fo: stylesheet-fo.xsl %.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) --stringparam paper.type A4 -o $@ $^

%-US.fo: stylesheet-fo.xsl %.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) --stringparam paper.type USletter -o $@ $^


##
## Experimental Texinfo targets
##

DB2X_TEXIXML = db2x_texixml
DB2X_XSLTPROC = db2x_xsltproc
MAKEINFO = makeinfo

%.texixml: %.xml
	$(DB2X_XSLTPROC) -s texi -g output-file=$(basename $@) $< -o $@

%.texi: %.texixml
	$(DB2X_TEXIXML) --encoding=iso-8859-1//TRANSLIT $< --to-stdout >$@

%.info: %.texi
	$(MAKEINFO) --enable-encoding --no-split --no-validate $< -o $@

# Cancel built-in suffix rules, interfering with PS building
.SUFFIXES:


##
## Check
##

# Quick syntax check without style processing
check: postgres.sgml $(ALMOSTALLSGML)
	$(NSGMLS) $(SPFLAGS) $(SGMLINCLUDE) -s $<


##
## Clean
##

clean distclean maintainer-clean:
# HTML
	rm -f *.html html-stamp
# man
	rm -rf man1 man7 man-stamp
# print
	rm -f *.rtf *.tex-ps *.tex-pdf *.dvi *.aux *.log *.ps *.pdf *.out *.fot
# index
	rm -f HTML.index $(GENERATED_SGML)
# text
	rm -f INSTALL HISTORY regress_README
# XSLT
	rm -f postgres.xml htmlhelp.hhp toc.hhc index.hhk *.fo
# Texinfo
	rm -f *.texixml *.texi *.info db2texi.refs
ifeq ($(vpath_build), yes)
	rm -f stylesheet.css
endif
