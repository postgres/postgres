#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/gist
#
# IDENTIFICATION
#    src/backend/access/gist/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/gist
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = gist.o gistutil.o gistxlog.o gistvacuum.o gistget.o gistscan.o \
       gistproc.o gistsplit.o gistbuild.o gistbuildbuffers.o

include $(top_srcdir)/src/backend/common.mk
