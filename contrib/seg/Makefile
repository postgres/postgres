# $Header: /cvsroot/pgsql/contrib/seg/Makefile,v 1.7.2.1 2003/01/31 20:58:28 tgl Exp $

subdir = contrib/seg
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = seg
OBJS = seg.o segparse.o buffer.o
DATA_built = seg.sql
DOCS = README.seg
REGRESS = seg


# segscan is compiled as part of segparse
segparse.o: segscan.c

segparse.c: segparse.h ;

# The sed hack is so that we can get the same error messages with
# bison 1.875 and later as we did with earlier bisons.  Eventually,
# I suppose, we should re-standardize on "syntax error" --- in which
# case flip the sed translation, but don't remove it.

segparse.h: segparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) -p seg_yy $<
	sed -e 's/"syntax error/"parse error/' < y.tab.c > segparse.c
	mv -f y.tab.h segparse.h
	rm -f y.tab.c
else
	@$(missing) bison $< $@
endif

segscan.c: segscan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -Pseg_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

EXTRA_CLEAN = segparse.c segparse.h segscan.c y.tab.c y.tab.h


include $(top_srcdir)/contrib/contrib-global.mk
