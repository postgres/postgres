#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/pgrepack
#
# IDENTIFICATION
#    src/backend/replication/pgrepack
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/pgrepack
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	pgrepack.o
PGFILEDESC = "pgrepack - logical replication output plugin for REPACK"
NAME = pgrepack

all: all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean: clean-lib
	rm -f $(OBJS)
