# Postgres documentation makefile
# Thomas Lockhart

PGDOCS= ..
SRCDIR= ../../src

HPATH=$(PGDOCS)/doc
PPATH=$(PGDOCS)/doc

#HSTYLE=/usr/lib/sgml/stylesheets/jade/docbook/html
#PSTYLE=/usr/lib/sgml/stylesheets/jade/docbook/print

HSTYLE=/home/tgl/SGML/db107.d/docbook/html
PSTYLE=/home/tgl/SGML/db107.d/docbook/print

HDSL=$(HSTYLE)/docbook.dsl
PDSL=$(PSTYLE)/docbook.dsl

#DBOPTS=-V %no-split-output% -V %no-make-index%

TAR= tar
TAREXCLUDE= --exclude=Makefile --exclude='*.sgml'

# Pick up Makefile.custom from the source area
# This is the only resource from the code source area and is optional

ifneq ($(wildcard $(SRCDIR)/Makefile.custom), )
include $(SRCDIR)/Makefile.custom
endif

TARGETS= postgres tutorial user admin programmer

HTARGETS=#make this a mapping from targets
PTARGETS=#make this a mapping from targets

.PRECIOUS: postgres.html postgres.tex postgres.dvi
.PHONY: sources clean

install::
	$(MAKE) all
	(mv -rf *.gz ..)

all:: $(SGO) $(SGP)

sources::
	($(TAR) zcf sources.tar.gz --exclude='*.htm*' --exclude='*.gz' .)

user.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml user.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml .)

tutorial.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml tutorial.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics clientserver.gif)

clean::
	(rm -rf *.html *.htm)

distclean::
	$(MAKE) -C sgml clean

# Generic production rules

# Compressed file

%.gz:	%
	(gzip -f $<)

# TAR file for HTML package

%.tar:	%.html # %.ps
	(tar cf $@ $*.html index.html *.htm *.gif) # $*.ps
	(rm -rf index.html *.htm)

#	(mkdir $*)
#	(rm -rf $*/*)
#	(mv *.htm $*/)
#	(cd $*/; ln -sf book01.htm index.html)
#	(tar cf $@ $*)

# HTML
# Include some softlinks to the generic default file names

%.html:	%.sgml $(HDSL)
	(rm -rf *.htm)
	jade $(DBOPTS) -D sgml -d $(HDSL) -t sgml $<
	(ln -sf book01.htm index.html)
	(ln -sf book01.htm $*.html)

#	(mkdir $(HPATH)/$*)	# be sure there is somewhere to put them
#	(rm -rf $(HPATH)/$*/*)	# remove existing files since some names may be obsolete
#	(mv *.htm $(HPATH)/$*/)	# and copy 'em over
#	(cd $(HPATH)/$*/; ln -sf book01.htm index.html)

# RTF to allow minor editing for hardcopy
# This is used for v6.3 docs

%.rtf: %.sgml $(PDSL)
	jade $(DBOPTS) -d $(PDSL) -t rtf $<

# TeX and DVI

%.tex: %.sgml $(PDSL)
	jade $(DBOPTS) -d $(PDSL) -t tex $<

%.dvi: %.tex
	jadetex $<
	jadetex $<

# Postscript from TeX

%.ps: %.dvi
	dvips -o $@ $<

# Graphics

%.gif:
	cp -p graphics/%.gif .
