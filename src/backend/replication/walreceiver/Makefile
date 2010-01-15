#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication/walreceiver
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/replication/walreceiver/Makefile,v 1.2 2010/01/15 20:34:11 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/replication/walreceiver
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS = walreceiver.o
SHLIB_LINK = $(libpq)
NAME = walreceiver

all: submake-libpq all-shared-lib

include $(top_srcdir)/src/Makefile.shlib

install: all installdirs install-lib

installdirs: installdirs-lib

uninstall: uninstall-lib

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS)
