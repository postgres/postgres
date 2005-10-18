# $PostgreSQL: pgsql/contrib/tsearch2/snowball/Makefile,v 1.8 2005/10/18 01:30:48 tgl Exp $

SUBOBJS = english_stem.o api.o russian_stem.o utilities.o

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

PG_CPPFLAGS = -I$(srcdir)/..

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2/snowball
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += $(CFLAGS_SL)

all: SUBSYS.o

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


