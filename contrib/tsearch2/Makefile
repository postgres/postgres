# $Header: /cvsroot/pgsql/contrib/tsearch2/Makefile,v 1.4 2003/08/23 04:25:28 petere Exp $

subdir = contrib/tsearch2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


MODULE_big = tsearch2
OBJS = dict_ex.o dict.o snmap.o stopword.o common.o prs_dcfg.o \
       dict_snowball.o dict_ispell.o dict_syn.o \
       wparser.o wparser_def.o \
       ts_cfg.o tsvector.o rewrite.o crc32.o query.o gistidx.o \
       tsvector_op.o rank.o ts_stat.o

SUBDIRS     := snowball ispell wordparser
SUBDIROBJS  := $(SUBDIRS:%=%/SUBSYS.o)

OBJS:= $(OBJS) $(SUBDIROBJS)

$(SUBDIROBJS): $(SUBDIRS:%=%-recursive) ;

$(SUBDIRS:%=%-recursive):
	$(MAKE) -C $(subst -recursive,,$@) SUBSYS.o

PG_CPPFLAGS = -I$(srcdir)/snowball -I$(srcdir)/ispell -I$(srcdir)/wordparser

DATA = stopword/english.stop stopword/russian.stop
DATA_built = tsearch2.sql untsearch2.sql
DOCS = README.tsearch2
REGRESS = tsearch2

EXTRA_CLEAN = tsearch2.sql.in

SHLIB_LINK := -lm

tsearch2.sql: tsearch.sql.in
	sed -e 's,MODULE_PATHNAME,$$libdir/$*,g' \
	    -e 's,DATA_PATH,$(datadir)/contrib,g' $< >$@

untsearch2.sql: untsearch.sql.in
	cp $< $@

.PHONY: subclean
clean: subclean

subclean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean || exit; done
 
include $(top_srcdir)/contrib/contrib-global.mk
