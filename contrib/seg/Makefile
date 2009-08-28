# $PostgreSQL: pgsql/contrib/seg/Makefile,v 1.22 2009/08/28 20:26:18 petere Exp $

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
segparse.o: segscan.c

segparse.c: segparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

segscan.c: segscan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: segparse.c segscan.c

maintainer-clean:
	rm -f segparse.c segscan.c
