#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# These files are used in other directories for portability on systems
# with broken/missing library files.

# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/port/Makefile,v 1.4 2003/06/14 14:35:42 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

ifdef LIBOBJS
all: libpgport.a
endif

libpgport.a: $(LIBOBJS)
	$(AR) crs $@ $^

thread.o: thread.c
	$(CC) $(CFLAGS) $(THREAD_CFLAGS) -c thread.c

clean distclean maintainer-clean:
	rm -f libpgport.a $(LIBOBJS)
