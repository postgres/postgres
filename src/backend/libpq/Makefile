#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/libpq/Makefile,v 1.39 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = be-fsstubs.o be-secure.o auth.o crypt.o hba.o ip.o md5.o pqcomm.o \
       pqformat.o pqsignal.o

include $(top_srcdir)/src/backend/common.mk
