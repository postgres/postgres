#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# These files are used in other directories for portability on systems
# with broken/missing library files.

# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/port/Makefile,v 1.3 2002/07/27 20:10:05 petere Exp $
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

clean distclean maintainer-clean:
	rm -f libpgport.a $(LIBOBJS)
