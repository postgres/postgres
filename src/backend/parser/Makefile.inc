#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the parser module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/parser/Attic/Makefile.inc,v 1.2.2.2 1996/09/21 07:47:48 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/parser

#PARSEYACCS= gram.c parse.h
PARSEYACCS= gram.c

$(PARSEYACCS): gram.y
	cd $(objdir) 
	$(YACC) $(YFLAGS) $<
	mv y.tab.c $(objdir)/gram.c
	mv y.tab.h $(objdir)/parse.h

$(objdir)/gram.o: gram.c
	$(cc_inobjdir)


scan.c:	scan.l
	cd $(objdir); $(LEX) $<; mv lex.yy.c scan.c

$(objdir)/scan.o: scan.c
	$(cc_inobjdir)


SRCS_PARSER+= analyze.c catalog_utils.c dbcommands.c gram.c \
	keywords.c parser.c parse_query.c scan.c scansup.c sysfunc.c

CLEANFILES+= scan.c ${PARSEYACCS}

POSTGRES_DEPEND+= scan.c $(PARSEYACCS)

HEADERS+= catalog_utils.h io.h parse_query.h parsetree.h \
	dbcommands.h keywords.h sysfunc.c


