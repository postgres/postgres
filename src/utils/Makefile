#-------------------------------------------------------------------------
#
# Makefile for utils
#
# $Header: /cvsroot/pgsql/src/utils/Attic/Makefile,v 1.8 2000/07/02 15:21:27 petere Exp $
#
# About strdup:  Some systems have strdup in their standard library, others
# don't.  Ones that don't will use this make file to compile the strdup.c
# in this directory.  (You don't see strdup mentioned because the implicit
# rule does the job).  We don't make strdup.o unless asked to explicitly
# because the strdup.c in this directory may conflict with the strdup()
# prototype from the system library and cause a compile error.
#
# getopt.o is likewise built only on request.
#
# dllinit.o is only built on Win32 platform.
#
#-------------------------------------------------------------------------

subdir = src/utils
top_builddir = ../..
include ../Makefile.global

all:

clean distclean maintainer-clean:
	rm -f dllinit.o getopt.o strdup.o

depend dep:
	$(CC) $(CFLAGS) -MM *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
