# $PostgreSQL: pgsql/contrib/seg/Makefile,v 1.20 2007/11/10 23:59:51 momjian Exp $

MODULE_big = seg
OBJS = seg.o segparse.o
DATA_built = seg.sql
DATA = uninstall_seg.sql
REGRESS = seg

EXTRA_CLEAN = y.tab.c y.tab.h

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
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
