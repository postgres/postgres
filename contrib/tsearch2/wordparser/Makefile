# $PostgreSQL: pgsql/contrib/tsearch2/wordparser/Makefile,v 1.5 2003/11/29 19:51:36 pgsql Exp $

subdir = contrib/tsearch2/wordparser
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


PG_CPPFLAGS = -I$(srcdir)/..
override CFLAGS += $(CFLAGS_SL)

SUBOBJS =  parser.o deflex.o

all: SUBSYS.o

parser.c: parser.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -8 -Ptsearch2_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

SUBSYS.o: $(SUBOBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS) parser.c

include $(top_srcdir)/contrib/contrib-global.mk
