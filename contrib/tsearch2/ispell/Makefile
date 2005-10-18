# $PostgreSQL: pgsql/contrib/tsearch2/ispell/Makefile,v 1.9 2005/10/18 01:30:48 tgl Exp $

SUBOBJS = spell.o regis.o 

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

PG_CPPFLAGS = -I$(srcdir)/.. 

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2/ispell
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += $(CFLAGS_SL)

all: SUBSYS.o

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


