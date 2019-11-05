#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/sort
#
# IDENTIFICATION
#    src/backend/utils/sort/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/sort
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

OBJS = \
	logtape.o \
	sharedtuplestore.o \
	sortsupport.o \
	tuplesort.o \
	tuplestore.o

tuplesort.o: qsort_tuple.c

qsort_tuple.c: gen_qsort_tuple.pl
	$(PERL) $(srcdir)/gen_qsort_tuple.pl $< > $@

include $(top_srcdir)/src/backend/common.mk

maintainer-clean:
	rm -f qsort_tuple.c
