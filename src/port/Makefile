#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# These files are used in other directories for portability on systems
# with broken/missing library files.

# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/port/Makefile,v 1.1 2002/07/18 04:13:59 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/port
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

OBJS=$(GETHOSTNAME) $(GETRUSAGE) $(INET_ATON) $(ISINF) $(MEMCMP) \
        $(MISSING_RANDOM) $(SNPRINTF) $(SRANDOM) $(STRCASECMP) $(STRDUP) \
	$(STRERROR) $(STRTOL) $(STRTOUL)

distclean clean:
	rm -f $(OBJS)
