#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/utils/Attic/Makefile,v 1.6 1999/01/17 06:20:03 momjian Exp $
#
# About strdup:  Some systems have strdup in their standard library, others
# don't.  Ones that don't will use this make file to compile the strdup.c
# in this directory.  (You don't see strdup mentioned because the implicit
# rule does the job).  We don't make strdup.o unless asked to explicitly
# because the strdup.c in this directory may conflict with the strdup()
# prototype from the system library and cause a compile error.
#
#-------------------------------------------------------------------------

SRCDIR = ..
include ../Makefile.global

all: version.o

install:

depend dep:
	$(CC) $(CFLAGS) -MM *.c >depend

clean: 
	rm -f version.o
ifeq ($(PORTNAME), win)
	rm -f dllinit.o
endif

ifeq (depend,$(wildcard depend))
include depend
endif

