# $PostgreSQL: pgsql/contrib/tsearch2/wordparser/Makefile,v 1.8 2005/10/18 01:30:49 tgl Exp $

SUBOBJS =  parser.o deflex.o

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS) parser.c

PG_CPPFLAGS = -I$(srcdir)/..

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2/wordparser
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += $(CFLAGS_SL)

all: SUBSYS.o

parser.c: parser.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -8 -Ptsearch2_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


