#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regex
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/regex/Makefile,v 1.10 1999/12/09 19:14:55 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..
CFLAGS += -DPOSIX_MISTAKE 

DEBUGOBJ =

OBJS = regcomp.o regerror.o regexec.o regfree.o

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
DEBUGOBJ += ../utils/mb/SUBSYS.o
endif

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) -o SUBSYS.o $(OBJS)

retest: retest.o SUBSYS.o $(DEBUGOBJ)
	$(CC) -o retest retest.o SUBSYS.o $(DEBUGOBJ)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) retest retest.o

ifeq (depend,$(wildcard depend))
include depend
endif

