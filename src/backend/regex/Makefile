#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regex
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/regex/Makefile,v 1.13 2000/05/29 05:44:58 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -DPOSIX_MISTAKE 

DEBUGOBJ =

OBJS = regcomp.o regerror.o regexec.o regfree.o

ifdef MULTIBYTE
DEBUGOBJ += ../utils/mb/SUBSYS.o
endif

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

retest: retest.o SUBSYS.o $(DEBUGOBJ)
	$(CC) -o retest retest.o SUBSYS.o $(DEBUGOBJ)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) retest retest.o

ifeq (depend,$(wildcard depend))
include depend
endif

