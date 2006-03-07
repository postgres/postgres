# $PostgreSQL: pgsql/contrib/seg/Makefile,v 1.16 2006/03/07 01:03:12 tgl Exp $

MODULE_big = seg
OBJS = seg.o segparse.o
DATA_built = seg.sql
DATA = uninstall_seg.sql
DOCS = README.seg
REGRESS = seg

EXTRA_CLEAN = segparse.c segparse.h segscan.c y.tab.c y.tab.h

PG_CPPFLAGS = -I.

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/seg
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


# segscan is compiled as part of segparse
segparse.o: segscan.c

# See notes in src/backend/parser/Makefile about the following two rules

segparse.c: segparse.h ;

segparse.h: segparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv -f y.tab.c segparse.c
	mv -f y.tab.h segparse.h
else
	@$(missing) bison $< $@
endif

segscan.c: segscan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif
