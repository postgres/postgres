#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gist
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/gist/Makefile,v 1.18 2008/02/19 10:30:06 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gist
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = gist.o gistutil.o gistxlog.o gistvacuum.o gistget.o gistscan.o \
       gistproc.o gistsplit.o

include $(top_srcdir)/src/backend/common.mk
