#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/regex
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/regex/Makefile,v 1.21 2003/11/29 19:51:55 pgsql Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/regex
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = regcomp.o regerror.o regexec.o regfree.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

# mark inclusion dependencies between .c files explicitly
regcomp.o: regcomp.c regc_lex.c regc_color.c regc_nfa.c regc_cvec.c regc_locale.c

regexec.o: regexec.c rege_dfa.c

clean: 
	rm -f SUBSYS.o $(OBJS)
