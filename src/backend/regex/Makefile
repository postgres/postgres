#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/regex
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/regex/Makefile,v 1.22 2008/02/19 10:30:08 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/regex
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = regcomp.o regerror.o regexec.o regfree.o

include $(top_srcdir)/src/backend/common.mk

# mark inclusion dependencies between .c files explicitly
regcomp.o: regcomp.c regc_lex.c regc_color.c regc_nfa.c regc_cvec.c regc_locale.c

regexec.o: regexec.c rege_dfa.c
