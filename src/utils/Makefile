#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/utils/Attic/Makefile,v 1.2 1996/11/11 16:30:07 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ..
include ../Makefile.global

INCLUDE_OPT = -I../include

CFLAGS+=$(INCLUDE_OPT)

all: version.o

install:

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f version.o

ifeq (depend,$(wildcard depend))
include depend
endif

