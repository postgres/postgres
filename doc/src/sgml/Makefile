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
#    $Header: /cvsroot/pgsql/doc/src/sgml/Makefile,v 1.26 2000/11/24 17:44:21 petere Exp $
#
#----------------------------------------------------------------------------

subdir = doc/src/sgml
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


ifndef DOCBOOKSTYLE
DOCBOOKSTYLE = /home/projects/pgsql/developers/thomas/db143.d/docbook
endif

ifndef HSTYLE
HSTYLE = $(DOCBOOKSTYLE)/html
endif
ifndef PSTYLE
PSTYLE = $(DOCBOOKSTYLE)/print
endif

HDSL=$(HSTYLE)/docbook.dsl
PDSL=$(PSTYLE)/docbook.dsl

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
	pg_config-ref.sgml \
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


##
## Man pages
##

.PHONY: man man1 manl

man:
	$(MAKE) man1 manl
	rm -r *.1 *.l man1 manl
	$(MAKE) man1 manl

manpage.refs:
	$(MAKE) man

man1: $(APPTARGETS) manpage.refs
	rm -rf man1
	if [ ! -d man1 ]; then mkdir man1; fi
	cp *.1 man1/

manl: $(SQLTARGETS) manpage.refs
	rm -rf manl/*
	if [ ! -d manl ]; then mkdir manl; fi
	cp *.l manl/

%.1: %-ref.sgml
	$(NSGMLS) $(NSGMLS_FLAGS) refentry.sgml $< \
	| $(SGMLSPL) $(D2MSCRIPT) --lowercase --section 1

%.1: %.sgml
	$(NSGMLS) $(NSGMLS_FLAGS) refentry.sgml $< \
	| $(SGMLSPL) $(D2MSCRIPT) --lowercase --section 1

%.l: %.sgml
	$(NSGMLS) $(NSGMLS_FLAGS) refentry.sgml $< \
	| $(SGMLSPL) $(D2MSCRIPT) --lowercase --section l


##
## HTML
##

JADE.html = $(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -d $(HDSL) -V %use-id-as-filename% -t sgml

postgres.html: postgres.sgml $(ALLSGML)
	@rm -f *.htm
	$(JADE.html) $<
# Include some softlinks to the generic default file names
	ln -sf postgres.htm index.html
	ln -sf postgres.htm postgres.html

$(addsuffix .html, $(ALLBOOKS)): %.html: %.sgml $(ALLSGML)
	@rm -f *.htm
	$(JADE.html) book-decl.sgml $<
	ln -sf $*.htm index.html
	ln -sf $*.htm $*.html


##
## Print
##

# RTF to allow minor editing for hardcopy
$(addsuffix .rtf, $(ALLBOOKS)): %.rtf: %.sgml $(ALLSGML)
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -d $(PDSL) -t rtf book-decl.sgml $<

# TeX and DVI
$(addsuffix .tex, $(ALLBOOKS)): %.tex: %.sgml $(ALLSGML)
	$(JADE) $(JADEFLAGS) $(SGMLINCLUDE) -d $(PDSL) -t tex book-decl.sgml $<

%.dvi: %.tex
	jadetex $<
	jadetex $<

# Postscript from TeX
%.ps: %.dvi
	dvips -o $@ $<

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
# HTML
	rm -f HTML.manifest *.html *.htm
# man
	rm -rf *.1 *.l man1 manl manpage.refs manpage.links manpage.log
# print
	rm -f *.rtf *.tex *.dvi *.aux *.log *.ps *.pdf
