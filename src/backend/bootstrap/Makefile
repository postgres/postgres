#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the bootstrap module
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/bootstrap/Makefile,v 1.12 1997/12/20 00:23:19 scrappy Exp $
#
#
# We must build bootparse.c and bootscanner.c with yacc and lex and sed,
# but bootstrap.c is part of the distribution.
#
# Another kinda weird Makefile cause we need two
#  scanner/parsers in the backend and most yaccs and lexs
#  don't have the prefix option.
#
#	sed files are HACK CITY! - redo...
#
#-------------------------------------------------------------------------

SRCDIR= ../..
include ../../Makefile.global

INCLUDE_OPT= -I.. 

CFLAGS+= $(INCLUDE_OPT)  

ifeq ($(CC), gcc)
CFLAGS+= -Wno-error
endif

BOOTYACCS= bootstrap_tokens.h bootparse.c

OBJS= bootparse.o bootscanner.o bootstrap.o 

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

# bootstrap.o's dependency on bootstrap_tokens.h is computed by the
# make depend, but we state it here explicitly anyway because 
# bootstrap_tokens.h doesn't even exist at first and if user fails to 
# do make depend, we still want the build to succeed.

bootstrap.o: bootstrap_tokens.h

bootstrap_tokens.h bootparse.c: bootparse.y
	$(YACC) $(YFLAGS) $<
	grep -v "^#" boot.sed > sedfile
	sed -f sedfile < y.tab.c > bootparse.c
	mv y.tab.h bootstrap_tokens.h
	rm -f y.tab.c sedfile

bootscanner.c: bootscanner.l
	$(LEX) $<
	grep -v "^#" boot.sed > sedfile
	sed -f sedfile < lex.yy.c > bootscanner.c
	rm -f lex.yy.c sedfile

clean:
	rm -f SUBSYS.o $(OBJS) bootparse.c bootstrap_tokens.h bootscanner.c
# And the garbage that might have been left behind by partial build:
	rm -f y.tab.h y.tab.c y.output lex.yy.c

# This is unusual:  We actually have to build some of the parts before
# we know what the header file dependencies are.  
dep: bootparse.c bootscanner.c bootstrap_tokens.h
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif

