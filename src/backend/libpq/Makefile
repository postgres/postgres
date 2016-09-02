#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for libpq subsystem (backend half of libpq interface)
#
# IDENTIFICATION
#    src/backend/libpq/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/libpq
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

# be-fsstubs is here for historical reasons, probably belongs elsewhere

OBJS = be-fsstubs.o be-secure.o auth.o crypt.o hba.o ifaddr.o pqcomm.o \
       pqformat.o pqmq.o pqsignal.o

ifeq ($(with_openssl),yes)
OBJS += be-secure-openssl.o
endif

include $(top_srcdir)/src/backend/common.mk
