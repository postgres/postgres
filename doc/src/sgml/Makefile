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
#    $Header: /cvsroot/pgsql/doc/src/sgml/Makefile,v 1.22 2000/10/05 19:48:16 momjian Exp $
#
#----------------------------------------------------------------------------

# Pick up Makefile.global from the source area
# This is the only resource from the code source area and is optional.
# Actually, we want this to get Makefile.custom - thomas 1998-03-01
subdir = doc/src/sgml
top_builddir = ../../..
-include $(top_builddir)/src/Makefile.global


# This is where the default stylesheets appear on my linux system.
# Probably no need to change this; rather, put definitions
#  for HSTYLE and PSTYLE and/or for HDSL and PDSL
#  in Makefile.custom in your code src directory.
#HSTYLE=/usr/lib/sgml/stylesheets/nwalsh-modular/html
#PSTYLE=/usr/lib/sgml/stylesheets/nwalsh-modular/print
# This is where the production stylesheets appear on postgresql.org
ifndef HSTYLE
HSTYLE= /home/projects/pgsql/developers/thomas/db143.d/docbook/html
endif
ifndef PSTYLE
PSTYLE= /home/projects/pgsql/developers/thomas/db143.d/docbook/print
endif

HDSL=$(HSTYLE)/docbook.dsl
PDSL=$(PSTYLE)/docbook.dsl

# Options for DocBook style sheets

DBOPTS= -D ref -D ../graphics
#DBOPTS+= -V %no-split-output% -V %no-make-index%
# use-id-as-filename picks up the "Id=" fields to use as file names.
# If this is omitted then file names are generic "c01.htm"
#  as in v6.3 documentation. Yuck.
DBOPTS+= -V %use-id-as-filename%

# docbook2man generates man pages from docbook refentry source code.
# This is where the (patched) docbook2man perl scripts reside on hub.org.

D2MDIR= /home/users/t/thomas/d2x/docbook2man
D2MSCRIPT= $(D2MDIR)/docbook2man-spec.pl

TAR= tar

# Hmm, made this optional but jade _really_ doesn't like them missing
# - thomas 1998-03-01
ifneq ($(HDSL), )
HTMLOPTS= -d $(HDSL)
endif
ifneq ($(PDSL), )
PRINTOPTS= -d $(PDSL)
endif

vpath %.sgml ./ref

MANSOURCES= $(wildcard ref/*.sgml)

APPLICATIONS= createdb.sgml createlang.sgml createuser.sgml \
	dropdb.sgml droplang.sgml dropuser.sgml \
	ecpg-ref.sgml \
	initdb.sgml initlocation.sgml \
	ipcclean.sgml \
	pg_dump.sgml \
	pg_dumpall.sgml \
	pg_passwd.sgml \
	pg_upgrade.sgml \
	pgaccess-ref.sgml \
	pgadmin-ref.sgml \
	pg-config-ref.sgml \
	pg_ctl-ref.sgml \
	pgtclsh.sgml \
	pgtksh.sgml \
	postgres-ref.sgml \
	postmaster.sgml \
	psql-ref.sgml \
	vacuumdb.sgml

COMMANDS= abort.sgml alter_group.sgml alter_table.sgml alter_user.sgml \
	begin.sgml \
	close.sgml cluster.sgml comment.sgml commit.sgml copy.sgml \
	create_aggregate.sgml create_constraint.sgml create_database.sgml \
	create_function.sgml create_group.sgml \
	create_index.sgml \
	create_language.sgml create_operator.sgml create_rule.sgml create_sequence.sgml \
	create_table.sgml create_table_as.sgml create_trigger.sgml create_type.sgml \
	create_user.sgml create_view.sgml \
	declare.sgml delete.sgml \
	drop_aggregate.sgml drop_database.sgml drop_function.sgml drop_group.sgml \
	drop_index.sgml \
	drop_language.sgml drop_operator.sgml drop_rule.sgml drop_sequence.sgml \
	drop_table.sgml drop_trigger.sgml drop_type.sgml drop_user.sgml drop_view.sgml \
	end.sgml explain.sgml fetch.sgml grant.sgml \
	insert.sgml listen.sgml load.sgml lock.sgml move.sgml \
	notify.sgml \
	reindex.sgml reset.sgml revoke.sgml rollback.sgml \
	select.sgml select_into.sgml set.sgml set_constraints.sgml set_transaction.sgml show.sgml \
	truncate.sgml unlisten.sgml update.sgml vacuum.sgml

FUNCTIONS= current_date.sgml current_time.sgml current_timestamp.sgml current_user.sgml

#APPSOURCES= $(addprefix ref/, $(APPLICATIONS))
#SQLSOURCES= $(addprefix ref/, $(COMMANDS))
APPSOURCES= $(APPLICATIONS)
SQLSOURCES= $(COMMANDS)
XAPPTARGETS= $(APPLICATIONS:.sgml=.1)
APPTARGETS= $(XAPPTARGETS:-ref.1=.1)
XSQLTARGETS= $(COMMANDS:.sgml=.l)
SQLTARGETS= $(XSQLTARGETS:-ref.l=.l)

.PRECIOUS: postgres.tex postgres.dvi
.PHONY: install all clean distclean

install:

all:

man:
	$(MAKE) man1 manl
	($(RM) -r *.1 *.l man1 manl)
	$(MAKE) man1 manl

manpage.refs:
	$(MAKE) man

man1: $(APPTARGETS) manpage.refs
	$(RM) -rf man1
	if [ ! -d man1 ]; then mkdir man1; fi
	cp *.1 man1/

manl: $(SQLTARGETS) manpage.refs
	$(RM) -rf manl/*
	if [ ! -d manl ]; then mkdir manl; fi
	cp *.l manl/

clean:
	(rm -rf HTML.manifest *.html *.htm *.1 *.l man1 manl manpage*)

distclean:
	$(MAKE) clean

#
# Generic production rules
#

%.1: %-ref.sgml
	nsgmls $(NSGMLS_FLAGS) refentry.sgml $< \
	| sgmlspl $(D2MSCRIPT) --lowercase --blockformat --section 1

%.1: %.sgml
	nsgmls $(NSGMLS_FLAGS) refentry.sgml $< \
	| sgmlspl $(D2MSCRIPT) --lowercase --blockformat --section 1

%.l: %.sgml
	nsgmls $(NSGMLS_FLAGS) refentry.sgml $< \
	| sgmlspl $(D2MSCRIPT) --lowercase --blockformat --section l

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
	cp -p ../graphics/$@ .


# Generation of the INSTALL text file. Not fully automated, but better
# than nothing.
.PHONY: INSTALL
INSTALL: INSTALL.html
	@echo "|";\
	 echo "| You should now take \`$<', save it as a text file in Netscape,";\
	 echo "| and put it in place of the existing \`INSTALL' file.";\
	 echo "|"
	@rm -f tempfile.html tempfile.sgml

INSTALL.html: tempfile.html
	sed -e 's/Chapter 1. *//g' < $< > $@

tempfile.html: tempfile.sgml
	jade -d $(HDSL) -V nochunks -t sgml $< > $@

tempfile.sgml: standalone-install.sgml installation.sgml
	cat $+ > $@
