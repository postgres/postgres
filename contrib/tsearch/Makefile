# $Header: /cvsroot/pgsql/contrib/tsearch/Attic/Makefile,v 1.1 2001/10/12 23:19:09 tgl Exp $

subdir = contrib/tsearch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = tsearch
OBJS = parser.o crc32.o morph.o txtidx.o query.o gistidx.o rewrite.o

DATA_built = tsearch.sql
DOCS = README.tsearch
REGRESS = tsearch

parser.c: parser.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -8 -Ptsearch_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

EXTRA_CLEAN = parser.c

include $(top_srcdir)/contrib/contrib-global.mk
# DO NOT DELETE
