# $PostgreSQL: pgsql/contrib/seg/Makefile,v 1.21 2008/08/29 13:02:32 petere Exp $

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

$(srcdir)/segparse.c: segparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

$(srcdir)/segscan.c: segscan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: $(srcdir)/segparse.c $(srcdir)/segscan.c

maintainer-clean:
	rm -f $(srcdir)/segparse.c $(srcdir)/segscan.c
