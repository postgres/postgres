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
#    $Header: /cvsroot/pgsql/doc/src/sgml/Makefile,v 1.55 2003/01/14 23:19:34 petere Exp $
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
SGMLINCLUDE = -D $(srcdir) -D $(srcdir)/ref

ifndef NSGMLS
NSGMLS = nsgmls
endif

ifndef SGMLSPL
SGMLSPL = sgmlspl
endif

CONVERT = convert
EPSTOPDF = epstopdf


# docbook2man generates man pages from docbook refentry source code.

ifndef D2MDIR
# This is where the (patched) docbook2man perl scripts reside on hub.org.
D2MDIR= /home/users/t/thomas/d2x/docbook2man
endif
D2MSCRIPT= $(D2MDIR)/docbook2man-spec.pl

vpath %.sgml ./ref

ALLBOOKS = admin developer programmer reference tutorial user

GENERATED_SGML = bookindex.sgml setindex.sgml version.sgml \
	features-supported.sgml features-unsupported.sgml

ALLSGML := $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml) $(GENERATED_SGML)

ifdef DOCBOOKSTYLE
CATALOG = -c $(DOCBOOKSTYLE)/catalog
endif

# Enable draft mode during development
ifneq (,$(findstring devel, $(VERSION)))
JADEFLAGS += -V draft-mode
endif


##
## Man pages
##

.PHONY: man

DEFAULTSECTION := $(sqlmansect_dummy)

man: $(ALLSGML)
	$(NSGMLS) $(NSGMLS_FLAGS) $(srcdir)/book-decl.sgml $(srcdir)/reference.sgml \
	  | $(SGMLSPL) $(D2MSCRIPT) --lowercase --section $(DEFAULTSECTION) --date "`date '+%Y-%m-%d'`"
# One more time, to resolve cross-references
	$(NSGMLS) $(NSGMLS_FLAGS) $(srcdir)/book-decl.sgml $(srcdir)/reference.sgml \
	  | $(SGMLSPL) $(D2MSCRIPT) --lowercase --section $(DEFAULTSECTION) --date "`date '+%Y-%m-%d'`"
	$(mkinstalldirs) man1 man$(DEFAULTSECTION)
	mv *.1 man1/
	mv *.$(DEFAULTSECTION) man$(DEFAULTSECTION)/


##
## HTML
##

JADE.html = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -i output-html -t sgml

postgres.html: postgres.sgml $(ALLSGML) stylesheet.dsl
	@rm -f *.html
	$(JADE.html) $<
	ln -sf index.html $@

$(addsuffix .html, $(ALLBOOKS)): %.html: %.sgml $(ALLSGML) stylesheet.dsl
	@rm -f *.html
	$(JADE.html) book-decl.sgml $<
	ln -sf index.html $@

programmer.html: catalogs.gif connections.gif
postgres.html: catalogs.gif connections.gif


COLLATEINDEX := $(PERL) $(COLLATEINDEX) -f -g -t 'Index'

ifeq (,$(wildcard HTML.index))
bookindex.sgml:
	$(COLLATEINDEX) -o $@ -N

setindex.sgml:
	$(COLLATEINDEX) -x -o $@ -N
else
bookindex.sgml: HTML.index
	$(COLLATEINDEX) -i 'bookindex' -o $@ $<

setindex.sgml: HTML.index
	$(COLLATEINDEX) -i 'setindex' -x -o $@ $<
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
$(addsuffix .rtf, $(ALLBOOKS)): %.rtf: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t rtf -V rtf-backend -i output-print book-decl.sgml $<

# TeX
# Regular TeX and pdfTeX have slightly differing requirements, so we
# need to distinguish the path we're taking.

$(addsuffix .tex-ps, $(ALLBOOKS)): %.tex-ps: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t tex -V tex-backend -i output-print -V texdvi-output -o $@ book-decl.sgml $<

programmer.tex-ps: catalogs.eps connections.eps

$(addsuffix .tex-pdf, $(ALLBOOKS)): %.tex-pdf: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t tex -V tex-backend -i output-print -V texpdf-output -o $@ book-decl.sgml $<

programmer.tex-pdf: catalogs.pdf connections.pdf

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
$(addsuffix .fot, $(ALLBOOKS)): %.fot: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) $(CATALOG) -d stylesheet.dsl -t fot -i output-print -o $@ book-decl.sgml $<


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
	$(PERL) -p -e 's/<H(1|2)$$/<H\1 align=center/g' $< | $(LYNX) -force_html -dump -nolist /dev/stdin >$@

INSTALL.html: standalone-install.sgml installation.sgml version.sgml
	$(JADE.text) -V nochunks standalone-install.sgml installation.sgml >$@

HISTORY.html: release.sgml
	( echo '<!doctype appendix PUBLIC "-//OASIS//DTD DocBook V3.1//EN">'; \
	  cat $< ) >tempfile_HISTORY.sgml
	$(JADE.text) -V nochunks tempfile_HISTORY.sgml >$@
	rm tempfile_HISTORY.sgml

regress_README.html: regress.sgml
	( echo '<!doctype chapter PUBLIC "-//OASIS//DTD DocBook V3.1//EN" ['; \
	  echo '<!entity % standalone-ignore "IGNORE"> ]>'; \
	  cat $< ) >tempfile_regress_README.sgml
	$(JADE.text) -V nochunks tempfile_regress_README.sgml >$@
	rm tempfile_regress_README.sgml


##
## Check
##

check: $(addprefix check-, $(ALLBOOKS)) check-postgres

# Quick syntax check without style processing
$(addprefix check-, $(ALLBOOKS)): check-%: %.sgml $(ALLSGML)
	$(NSGMLS) $(SGMLINCLUDE) -s book-decl.sgml $<

check-postgres: postgres.sgml $(ALLSGML)
	$(NSGMLS) $(SGMLINCLUDE) -s $<


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
