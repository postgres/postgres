#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for parser
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/parser/Makefile,v 1.13 1997/12/20 00:24:55 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

INCLUDE_OPT= -I.. 

CFLAGS+= $(INCLUDE_OPT)

ifeq ($(CC), gcc)
CFLAGS+= -Wno-error
endif


OBJS= analyze.o gram.o keywords.o parser.o parse_agg.o parse_clause.o \
      parse_expr.o parse_func.o parse_node.o parse_oper.o parse_relation.o \
      parse_type.o parse_target.o scan.o scansup.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

gram.c parse.h: gram.y
	$(YACC) $(YFLAGS) $<
	mv y.tab.c gram.c
	mv y.tab.h parse.h

scan.c:	scan.l
	$(LEX) $<
	mv lex.yy.c scan.c

# The following dependencies on parse.h are computed by
# make depend, but we state them here explicitly anyway because 
# parse.h doesn't even exist at first and if user fails to 
# do make depend, we still want the build to succeed.

analyze.o keywords.o scan.o: parse.h

# This is unusual:  We actually have to build some of the parts before
# we know what the header file dependencies are.  
dep: gram.c scan.c
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

# Remove scan.c from the clean since we want to avoid rebuilding when using
# the original source distribution. This should help Solaris machines whose
# lex has trouble with exclusive states.
clean: 
	rm -f SUBSYS.o $(OBJS) gram.c parse.h # scan.c
# And the garbage that might have been left behind by partial build:
	rm -f y.tab.c y.tab.h lex.yy.c

ifeq (depend,$(wildcard depend))
include depend
endif

