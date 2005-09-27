# $PostgreSQL: pgsql/contrib/tsearch2/wordparser/Makefile,v 1.7 2005/09/27 17:13:12 tgl Exp $


PG_CPPFLAGS = -I$(srcdir)/..
SUBOBJS =  parser.o deflex.o
EXTRA_CLEAN = SUBSYS.o $(SUBOBJS) parser.c

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

parser.c: parser.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -8 -Ptsearch2_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^


