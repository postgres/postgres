# $Header: /cvsroot/pgsql/contrib/tsearch2/ispell/Attic/Makefile,v 1.3.2.1 2003/09/07 04:36:39 momjian Exp $

subdir = contrib/tsearch2/ispell
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PG_CPPFLAGS = -I$(srcdir)/.. $(CPPFLAGS)
override CFLAGS += $(CFLAGS_SL)

SUBOBJS = spell.o 

all: SUBSYS.o

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

include $(top_srcdir)/contrib/contrib-global.mk
