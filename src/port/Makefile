#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# These files are used in other directories for portability on systems
# with broken/missing library files.

# IDENTIFICATION
#    $PostgreSQL: pgsql/src/port/Makefile,v 1.10 2004/04/23 18:15:55 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

ifdef LIBOBJS
all: libpgport.a
endif

libpgport.a: $(LIBOBJS)
	$(AR) $(AROPT) $@ $^

thread.o: thread.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(PTHREAD_CFLAGS) -c $<

clean distclean maintainer-clean:
	rm -f libpgport.a $(LIBOBJS)
