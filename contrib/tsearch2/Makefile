# $PostgreSQL: pgsql/contrib/tsearch2/Makefile,v 1.17 2006/09/11 15:14:46 tgl Exp $

MODULE_big = tsearch2
OBJS = dict_ex.o dict.o snmap.o stopword.o common.o prs_dcfg.o \
       dict_snowball.o dict_ispell.o dict_syn.o dict_thesaurus.o \
       wparser.o wparser_def.o \
       ts_cfg.o tsvector.o query_cleanup.o crc32.o query.o gistidx.o \
       tsvector_op.o rank.o ts_stat.o \
       query_util.o query_support.o query_rewrite.o query_gist.o \
       ts_locale.o ts_lexize.o ginidx.o

SUBDIRS     := snowball ispell wordparser
SUBDIROBJS  := $(SUBDIRS:%=%/SUBSYS.o)

OBJS	+= $(SUBDIROBJS)

PG_CPPFLAGS = -I$(srcdir)/snowball -I$(srcdir)/ispell -I$(srcdir)/wordparser

DATA = stopword/english.stop stopword/russian.stop stopword/russian.stop.utf8 thesaurus
DATA_built = tsearch2.sql uninstall_tsearch2.sql
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

.PHONY: $(SUBDIRS:%=%-recursive)

$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

tsearch2.sql: tsearch.sql.in
	sed -e 's,MODULE_PATHNAME,$$libdir/$(MODULE_big),g' $< >$@

uninstall_tsearch2.sql: untsearch.sql.in
	cp $< $@

.PHONY: subclean
clean: subclean

subclean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean || exit; done
