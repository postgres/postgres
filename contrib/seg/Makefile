# $Header: /cvsroot/pgsql/contrib/seg/Makefile,v 1.9 2003/05/14 03:27:22 tgl Exp $

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

segparse.h: segparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) -p seg_yy $<
	mv -f y.tab.c segparse.c
	mv -f y.tab.h segparse.h
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
