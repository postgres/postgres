#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/libpqwalreceiver
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/replication/libpqwalreceiver/Makefile,v 1.2 2010/01/20 20:34:51 heikki Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/libpqwalreceiver
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = libpqwalreceiver.o
SHLIB_LINK = $(libpq)
NAME = libpqwalreceiver

all: submake-libpq all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
