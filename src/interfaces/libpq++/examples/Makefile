#-------------------------------------------------------------------------
#
# Makefile for libpq++ examples
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/interfaces/libpq++/examples/Attic/Makefile,v 1.14 2002/06/20 20:29:54 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/interfaces/libpq++/examples
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

LIBNAME= libpq++
HEADERDIR= $(includedir)
LIBPQDIR= $(libdir)

CXXFLAGS= $(CFLAGS)

CXXFLAGS+= -I$(HEADERDIR)

LDFLAGS+= -L$(LIBPQDIR) -lpq++ 

PROGS=	testlibpq0 testlibpq1 testlibpq2 testlibpq3 \
	testlibpq4 testlibpq5 testlibpq6 testlo

all: $(PROGS)

$(PROGS): % : %.cc
	$(CXX) $(CXXFLAGS) -o $@ $@.cc $(LDFLAGS)

.PHONY: submake

clean: 
	rm -f $(PROGS)

