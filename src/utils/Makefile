#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/utils/Attic/Makefile,v 1.7 2000/02/27 01:18:46 tgl Exp $
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

SRCDIR = ..
include $(SRCDIR)/Makefile.global

all: version.o

install:

depend dep:
	$(CC) $(CFLAGS) -MM *.c >depend

clean: 
	rm -f dllinit.o getopt.o strdup.o version.o

# Make sure version.o has proper dependency on version.h,
# even if we didn't do make depend.
version.o: version.c $(SRCDIR)/include/version.h

ifeq (depend,$(wildcard depend))
include depend
endif

