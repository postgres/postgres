# $PostgreSQL: pgsql/contrib/tsearch/Makefile,v 1.5 2004/08/20 20:13:08 momjian Exp $

PG_CPPFLAGS = -I.

MODULE_big = tsearch
OBJS = crc32.o morph.o txtidx.o query.o gistidx.o rewrite.o

DATA_built = tsearch.sql
DOCS = README.tsearch
REGRESS = tsearch

EXTRA_CLEAN = parser.c

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


# parser is compiled as part of query
query.o: parser.c

parser.c: parser.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -8 -Ptsearch_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

# DO NOT DELETE
