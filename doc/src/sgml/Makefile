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
#    $Header: /cvsroot/pgsql/doc/src/sgml/Makefile,v 1.5 1998/09/25 13:41:25 thomas Exp $
#
#----------------------------------------------------------------------------

PGDOCS= ../..
SRCDIR= ../../../src

# This is where the default stylesheets appear on my system.
# Probably no need to change this; rather, put definitions
#  for HSTYLE and PSTYLE and/or for HDSL and PDSL
#  in Makefile.custom in your code src directory.

HSTYLE=/usr/lib/sgml/stylesheets/jade/docbook/html
PSTYLE=/usr/lib/sgml/stylesheets/jade/docbook/print

#HSTYLE=/home/tgl/SGML/db107.d/docbook/html
#PSTYLE=/home/tgl/SGML/db107.d/docbook/print

HDSL=$(HSTYLE)/docbook.dsl
PDSL=$(PSTYLE)/docbook.dsl

# Options for DocBook style sheets

DBOPTS= -D ref -D ../graphics
#DBOPTS+= -V %no-split-output% -V %no-make-index%
# use-id-as-filename picks up the "Id=" fields to use as file names.
# If this is omitted then file names are generic "c01.htm"
#  as in v6.3 documentation. Yuck.
DBOPTS+= -V %use-id-as-filename%

TAR= tar

# Pick up Makefile.global from the source area
# This is the only resource from the code source area and is optional.
# Actually, we want this to get Makefile.custom - thomas 1998-03-01

ifneq ($(wildcard $(SRCDIR)/Makefile.global), )
include $(SRCDIR)/Makefile.global
endif

# Hmm, made this optional but jade _really_ doesn't like them missing
# - thomas 1998-03-01
ifneq ($(HDSL), )
HTMLOPTS= -d $(HDSL)
endif
ifneq ($(PDSL), )
PRINTOPTS= -d $(PDSL)
endif

.PRECIOUS: postgres.tex postgres.dvi
.PHONY: install all clean distclean

install::
#	$(MAKE) all
#	(mv -rf *.gz ..)

all::

clean::
	(rm -rf *.html *.htm)

distclean::
	$(MAKE) clean

#
# Generic production rules
#

# Compressed file

%.gz:	%
	(gzip -f $<)

# TAR file for HTML package

%.tar:	%.html # %.ps
	($(TAR) cf $@ $*.html index.html *.htm *.gif) # $*.ps
	(rm -rf index.html *.htm)

# HTML
# Include some softlinks to the generic default file names

%.html:	%.sgml #$(HDSL)
	(rm -rf *.htm)
	jade $(DBOPTS) $(HTMLOPTS) -t sgml $<
	(ln -sf $*.htm index.html)
	(ln -sf $*.htm $*.html)

# RTF to allow minor editing for hardcopy
# This is used for v6.3 docs

%.rtf: %.sgml #$(PDSL)
	jade $(DBOPTS) $(PRINTOPTS) -t rtf $<

# TeX and DVI

%.tex: %.sgml #$(PDSL)
	jade $(DBOPTS) $(PRINTOPTS) -t tex $<

%.dvi: %.tex
	jadetex $<
	jadetex $<

# Postscript from TeX

%.ps: %.dvi
	dvips -o $@ $<

# Graphics

%.gif:
	cp -p ../graphics/%.gif .

