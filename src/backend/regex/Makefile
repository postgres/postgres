#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for regex
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/regex/Makefile,v 1.6 1998/04/06 00:24:39 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..
CFLAGS += -DPOSIX_MISTAKE 

OBJS = regcomp.o regerror.o regexec.o regfree.o
ifdef MB
OBJS += utils.o wstrcmp.o wstrncmp.o
CFLAGS += -DMB=$(MB)
endif

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

