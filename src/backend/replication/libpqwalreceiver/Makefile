#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/libpqwalreceiver
#
# IDENTIFICATION
#    src/backend/replication/libpqwalreceiver/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/libpqwalreceiver
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = libpqwalreceiver.o $(WIN32RES)
SHLIB_LINK = $(libpq) $(filter -lintl, $(LIBS))
SHLIB_PREREQS = submake-libpq
PGFILEDESC = "libpqwalreceiver - receive WAL during streaming replication"
NAME = libpqwalreceiver

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
