# $PostgreSQL: pgsql/contrib/tsearch2/ispell/Makefile,v 1.5 2003/11/29 19:51:36 pgsql Exp $

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
