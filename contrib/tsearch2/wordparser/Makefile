# $Header: /cvsroot/pgsql/contrib/tsearch2/wordparser/Attic/Makefile,v 1.3.2.1 2003/09/07 04:36:39 momjian Exp $

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
