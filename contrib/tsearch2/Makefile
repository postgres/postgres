# $PostgreSQL: pgsql/contrib/tsearch2/Makefile,v 1.10 2005/09/27 17:13:11 tgl Exp $

MODULE_big = tsearch2
OBJS = dict_ex.o dict.o snmap.o stopword.o common.o prs_dcfg.o \
       dict_snowball.o dict_ispell.o dict_syn.o \
       wparser.o wparser_def.o \
       ts_cfg.o tsvector.o rewrite.o crc32.o query.o gistidx.o \
       tsvector_op.o rank.o ts_stat.o

SUBDIRS     := snowball ispell wordparser
SUBDIROBJS  := $(SUBDIRS:%=%/SUBSYS.o)

OBJS	+= $(SUBDIROBJS)

PG_CPPFLAGS = -I$(srcdir)/snowball -I$(srcdir)/ispell -I$(srcdir)/wordparser

DATA = stopword/english.stop stopword/russian.stop
DATA_built = tsearch2.sql untsearch2.sql
DOCS = README.tsearch2
REGRESS = tsearch2

SHLIB_LINK += $(filter -lm, $(LIBS))


ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/tsearch2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

tsearch2.sql: tsearch.sql.in
	sed -e 's,MODULE_PATHNAME,$$libdir/$(MODULE_big),g' $< >$@

untsearch2.sql: untsearch.sql.in
	cp $< $@

.PHONY: subclean
clean: subclean

subclean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean || exit; done
