#----------------------------------------------------------------------------
#
# PostgreSQL documentation makefile
#
# $PostgreSQL: pgsql/doc/src/sgml/Makefile,v 1.67 2004/03/05 19:48:03 momjian Exp $
#
#----------------------------------------------------------------------------

subdir = doc/src/sgml
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

.SECONDARY:
.NOTPARALLEL:

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

ifndef SGMLSPL
SGMLSPL = sgmlspl
endif

CONVERT = convert
EPSTOPDF = epstopdf

# docbook2man generates man pages from docbook refentry source code.
D2MSCRIPT= $(D2MDIR)/docbook2man-spec.pl


GENERATED_SGML = bookindex.sgml version.sgml \
	features-supported.sgml features-unsupported.sgml

ALLSGML := $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml) $(GENERATED_SGML)

ifdef DOCBOOKSTYLE
CATALOG = -c $(DOCBOOKSTYLE)/catalog
endif

# Enable draft mode during development
ifneq (,$(findstring devel, $(VERSION)))
override JADEFLAGS += -V draft-mode
endif

# Enable some extra warnings
override SPFLAGS += -wall -wno-unused-param -wno-empty

##
## Man pages
##

.PHONY: man

DEFAULTSECTION := $(sqlmansect_dummy)

man: postgres.sgml $(ALLSGML)
	$(NSGMLS) $(NSGMLS_FLAGS) $< | $(SGMLSPL) $(D2MSCRIPT) --lowercase --section $(DEFAULTSECTION) --date "`date '+%Y-%m-%d'`"
# One more time, to resolve cross-references
	$(NSGMLS) $(NSGMLS_FLAGS) $< | $(SGMLSPL) $(D2MSCRIPT) --lowercase --section $(DEFAULTSECTION) --date "`date '+%Y-%m-%d'`"
	$(mkinstalldirs) man1 man$(DEFAULTSECTION)
	mv *.1 man1/
	mv *.$(DEFAULTSECTION) man$(DEFAULTSECTION)/


##
## HTML
##

all: html

.PHONY: html

html: postgres.sgml $(ALLSGML) stylesheet.dsl
	@rm -f *.html
	$(JADE) $(JADEFLAGS) $(SPFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -i output-html -t sgml $<
ifeq ($(vpath_build), yes)
	@cp $(srcdir)/stylesheet.css .
endif


COLLATEINDEX := LC_ALL=C $(PERL) $(COLLATEINDEX) -f -g

ifeq (,$(wildcard HTML.index))
bookindex.sgml:
	$(COLLATEINDEX) -o $@ -N
else
bookindex.sgml: HTML.index
	$(COLLATEINDEX) -i 'bookindex' -o $@ $<
endif

version.sgml: $(top_builddir)/src/Makefile.global
	{ \
	  echo "<!entity version \"$(VERSION)\">"; \
	  echo "<!entity majorversion \"`expr $(VERSION) : '\([0-9][0-9]*\.[0-9][0-9]*\)'`\">"; \
	} >$@

features-supported.sgml: $(top_srcdir)/src/backend/catalog/sql_feature_packages.txt $(top_srcdir)/src/backend/catalog/sql_features.txt
	$(PERL) $(srcdir)/mk_feature_tables.pl YES $^ > $@

features-unsupported.sgml: $(top_srcdir)/src/backend/catalog/sql_feature_packages.txt $(top_srcdir)/src/backend/catalog/sql_features.txt
	$(PERL) $(srcdir)/mk_feature_tables.pl NO $^ > $@


##
## Print
##

# RTF to allow minor editing for hardcopy

%.rtf: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t rtf -V rtf-backend -i output-print $<

# TeX
# Regular TeX and pdfTeX have slightly differing requirements, so we
# need to distinguish the path we're taking.

%.tex-ps: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t tex -V tex-backend -i output-print -V texdvi-output -o $@ $<

%.tex-pdf: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t tex -V tex-backend -i output-print -V texpdf-output -o $@ $<

%.dvi: %.tex-ps
	@rm -f $*.aux $*.log
	jadetex $<
	jadetex $<
	jadetex $<

# Postscript from TeX
%.ps: %.dvi
	dvips -o $@ $<

%.pdf: %.tex-pdf
	@rm -f $*.aux $*.log $*.out
	pdfjadetex $<
	pdfjadetex $<
	pdfjadetex $<


# This generates an XML version of the flow-object tree.  It's useful
# for debugging DSSSL code, and possibly to interface to some other
# tools that can make use of this.
%.fot: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t fot -i output-print -o $@ $<


# Graphics

%.gif:
	cp $(srcdir)/../graphics/$@ .

%.eps: %.gif
	$(CONVERT) $< eps:$@

%.pdf: %.eps
	$(EPSTOPDF) $<


##
## Semi-automatic generation of some text files.
##

JADE.text = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -i output-text -t sgml
LYNX = lynx

INSTALL HISTORY regress_README: % : %.html
	$(PERL) -p -e 's/<H(1|2)$$/<H\1 align=center/g' $< | $(LYNX) -force_html -dump -nolist - >$@

INSTALL.html: standalone-install.sgml installation.sgml version.sgml
	$(JADE.text) -V nochunks standalone-install.sgml installation.sgml >$@

HISTORY.html: release.sgml
	( echo '<!doctype appendix PUBLIC "-//OASIS//DTD DocBook V4.2//EN">'; \
	  cat $< ) >tempfile_HISTORY.sgml
	$(JADE.text) -V nochunks tempfile_HISTORY.sgml >$@
	rm tempfile_HISTORY.sgml

regress_README.html: regress.sgml
	( echo '<!doctype chapter PUBLIC "-//OASIS//DTD DocBook V4.2//EN" ['; \
	  echo '<!entity % standalone-ignore "IGNORE"> ]>'; \
	  cat $< ) >tempfile_regress_README.sgml
	$(JADE.text) -V nochunks tempfile_regress_README.sgml >$@
	rm tempfile_regress_README.sgml


##
## Experimental XML stuff
##

OSX = osx # (may be called sx or sgml2xml on some systems)
XSLTPROC = xsltproc

postgres.xml: postgres.sgml $(GENERATED_SGML)
	$(OSX) -x lower $< | \
	  sed -e 's/\[\(lt\|gt\|amp\|nbsp\|copy\|quot\|ouml\|uuml\|egrave\) *\]/\&\1;/g' \
	      -e '1a\' -e '<!DOCTYPE book PUBLIC "-//OASIS//DTD DocBook XML V4.2//EN" "http://www.oasis-open.org/docbook/xml/4.2/docbookx.dtd">' \
	  >$@

testxml: stylesheet.xsl postgres.xml
	$(XSLTPROC) $(XSLTPROCFLAGS) --stringparam pg.version '$(VERSION)' $^


##
## Check
##

# Quick syntax check without style processing
check: postgres.sgml $(ALLSGML)
	$(NSGMLS) $(SPFLAGS) $(SGMLINCLUDE) -s $<


##
## Clean
##

clean distclean maintainer-clean:
# HTML
	rm -f HTML.manifest *.html *.gif
# man
	rm -rf *.1 *.$(DEFAULTSECTION) man1 man$(DEFAULTSECTION) manpage.refs manpage.links manpage.log
# print
	rm -f *.rtf *.tex-ps *.tex-pdf *.dvi *.aux *.log *.ps *.pdf *.out *.eps *.fot
# index
	rm -f HTML.index $(GENERATED_SGML)
