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
#    $Header: /cvsroot/pgsql/doc/src/sgml/Makefile,v 1.46 2001/12/08 03:24:21 thomas Exp $
#
#----------------------------------------------------------------------------

subdir = doc/src/sgml
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

.SECONDARY:
.NOTPARALLEL:

ifndef DOCBOOKSTYLE
DOCBOOKSTYLE = /home/projects/pgsql/developers/thomas/db162.d/docbook
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

ALLSGML := $(wildcard $(srcdir)/*.sgml $(srcdir)/ref/*.sgml) bookindex.sgml setindex.sgml

CATALOG = $(DOCBOOKSTYLE)/catalog


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

JADE.html = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -c $(CATALOG) -d stylesheet.dsl -i output-html -t sgml

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


#COLLATEINDEX = $(PERL) $(DOCBOOKSTYLE)/bin/collateindex.pl -f -g -t 'Index'
COLLATEINDEX = $(PERL) /usr/bin/collateindex.pl -f -g -t 'Index'

ifeq (,$(wildcard HTML.index))
bookindex.sgml:
ifeq (,$(wildcard $(COLLATEINDEX)))
	touch $@
else
	$(COLLATEINDEX) -o $@ -N
endif

setindex.sgml:
ifeq (,$(wildcard $(COLLATEINDEX)))
	touch $@
else
	$(COLLATEINDEX) -x -o $@ -N
endif
else
bookindex.sgml: HTML.index
	$(COLLATEINDEX) -i 'bookindex' -o $@ $<

setindex.sgml: HTML.index
	$(COLLATEINDEX) -i 'setindex' -x -o $@ $<
endif


##
## Print
##

# RTF to allow minor editing for hardcopy
$(addsuffix .rtf, $(ALLBOOKS)): %.rtf: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -c $(CATALOG) -d stylesheet.dsl -t rtf -V rtf-backend -i output-print book-decl.sgml $<

# TeX
# Regular TeX and pdfTeX have slightly differing requirements, so we
# need to distinguish the path we're taking.

$(addsuffix .tex-ps, $(ALLBOOKS)): %.tex-ps: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -c $(CATALOG) -d stylesheet.dsl -t tex -V tex-backend -i output-print -V texdvi-output -o $@ book-decl.sgml $<
# TeX transforms '--' into en-dashes.  This hack should go away when
# OpenJade is fixed (ver. 1.4?)
	$(PERL) -pi -e 's/\-\-/\\mbox{}-\\mbox{}-\\mbox{}/g' $@
	$(PERL) -pi -e 's/\<\</\\mbox{}<\\mbox{}<\\mbox{}/g' $@
	$(PERL) -pi -e 's/\>\>/\\mbox{}>\\mbox{}>\\mbox{}/g' $@
	$(PERL) -pi -e "s/\'\'/\\\\mbox{}'\\\\mbox{}'\\\\mbox{}/g" $@

programmer.tex-ps: catalogs.eps connections.eps

$(addsuffix .tex-pdf, $(ALLBOOKS)): %.tex-pdf: %.sgml $(ALLSGML) stylesheet.dsl
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -c $(CATALOG) -d stylesheet.dsl -t tex -V tex-backend -i output-print -V texpdf-output -o $@ book-decl.sgml $<
	$(PERL) -pi -e 's/\-\-/\\mbox{}-\\mbox{}-\\mbox{}/g' $@
	$(PERL) -pi -e 's/\<\</\\mbox{}<\\mbox{}<\\mbox{}/g' $@
	$(PERL) -pi -e 's/\>\>/\\mbox{}>\\mbox{}>\\mbox{}/g' $@
	$(PERL) -pi -e "s/\'\'/\\\\mbox{}'\\\\mbox{}'\\\\mbox{}/g" $@

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
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -c $(CATALOG) -d stylesheet.dsl -t fot -i output-print -o $@ book-decl.sgml $<


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

JADE.text = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -c $(CATALOG) -d stylesheet.dsl -i output-text -t sgml

INSTALL HISTORY regress_README: % : %.html
	@echo "|";\
	 echo "| You should now take '$<', save it as a text file in Netscape,";\
	 echo "| and put it in place of the existing '$@' file.";\
	 echo "|"

INSTALL.html: standalone-install.sgml installation.sgml
	$(JADE.text) -V nochunks $+ >$@

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
$(addprefix check-, $(ALLBOOKS)): check-%: %.sgml
	$(NSGMLS) $(SGMLINCLUDE) -s book-decl.sgml $<

check-postgres: postgres.sgml
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
	rm -f HTML.index bookindex.sgml setindex.sgml
