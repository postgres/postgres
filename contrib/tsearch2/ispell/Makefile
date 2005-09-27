# $PostgreSQL: pgsql/contrib/tsearch2/ispell/Makefile,v 1.8 2005/09/27 17:13:11 tgl Exp $

PG_CPPFLAGS = -I$(srcdir)/.. 
SUBOBJS = spell.o regis.o 
EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += $(CFLAGS_SL)

all: SUBSYS.o

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


