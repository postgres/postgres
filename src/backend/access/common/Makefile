#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/common
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/access/common/Makefile,v 1.24 2008/02/19 10:30:06 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/common
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = heaptuple.o indextuple.o printtup.o reloptions.o scankey.o tupdesc.o  

all: SUBSYS.o

include $(top_srcdir)/src/backend/common.mk
