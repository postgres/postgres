#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for parser
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/parser/Makefile,v 1.4 1996/11/14 07:33:30 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

INCLUDE_OPT= -I.. \
             -I../port/$(PORTNAME) \
             -I../../include

CFLAGS+= $(INCLUDE_OPT)

ifeq ($(CC), gcc)
# Until we figure out how to get rid of the warnings in this directory,
# we must turn off any -Werror that is in CFLAGS now.  These options only
# exist for the gcc compiler.
CFLAGS+= -Wno-error
endif

OBJS= analyze.o catalog_utils.o dbcommands.o gram.o \
      keywords.o parser.o parse_query.o scan.o scansup.o sysfunc.o

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

clean: 
	rm -f SUBSYS.o $(OBJS) gram.c parse.h scan.c
# And the garbage that might have been left behind by partial build:
	rm -f y.tab.c y.tab.h lex.yy.c

ifeq (depend,$(wildcard depend))
include depend
endif

