#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/utils/Attic/Makefile,v 1.4 1998/02/27 02:41:21 scrappy Exp $
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
	$(CC) -MM *.c >depend

clean: 
	rm -f version.o

ifeq (depend,$(wildcard depend))
include depend
endif

