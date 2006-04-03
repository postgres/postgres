# $PostgreSQL: pgsql/contrib/seg/Makefile,v 1.17 2006/04/03 18:47:41 petere Exp $

MODULE_big = seg
OBJS = seg.o segparse.o
DATA_built = seg.sql
DATA = uninstall_seg.sql
DOCS = README.seg
REGRESS = seg

EXTRA_CLEAN = y.tab.c y.tab.h

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
segparse.o: $(srcdir)/segscan.c

# See notes in src/backend/parser/Makefile about the following two rules

$(srcdir)/segparse.c: $(srcdir)/segparse.h ;

$(srcdir)/segparse.h: segparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv -f y.tab.c $(srcdir)/segparse.c
	mv -f y.tab.h $(srcdir)/segparse.h
else
	@$(missing) bison $< $@
endif

$(srcdir)/segscan.c: segscan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: $(srcdir)/segparse.c $(srcdir)/segparse.h $(srcdir)/segscan.c

maintainer-clean:
	rm -f $(srcdir)/segparse.c $(srcdir)/segparse.h $(srcdir)/segscan.c
