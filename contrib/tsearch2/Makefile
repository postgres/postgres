subdir = contrib/tsearch2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I. -I./snowball -I./ispell -I./wordparser $(CPPFLAGS)

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

DATA_built = tsearch2.sql untsearch2.sql
DOCS = README.tsearch2
REGRESS = tsearch2

EXTRA_CLEAN = tsearch2.sql.in

SHLIB_LINK := -lm
include $(top_srcdir)/contrib/contrib-global.mk
# DO NOT DELETE

install: installstop

installstop:
	cp stopword/*.stop $(datadir)


tsearch2.sql.in: tsearch.sql._in
	sed 's,DATA_PATH,$(datadir),g' < $< > $@

untsearch2.sql: untsearch.sql.in
	cp $< $@

clean: subclean

subclean:
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean || exit; done
 
