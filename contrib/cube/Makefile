# $PostgreSQL: pgsql/contrib/cube/Makefile,v 1.22 2008/08/29 13:02:32 petere Exp $

MODULE_big = cube
OBJS= cube.o cubeparse.o

DATA_built = cube.sql
DATA = uninstall_cube.sql
REGRESS = cube

EXTRA_CLEAN = y.tab.c y.tab.h

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/cube
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


# cubescan is compiled as part of cubeparse
cubeparse.o: $(srcdir)/cubescan.c

$(srcdir)/cubeparse.c: cubeparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

$(srcdir)/cubescan.c: cubescan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: $(srcdir)/cubeparse.c $(srcdir)/cubescan.c

maintainer-clean:
	rm -f $(srcdir)/cubeparse.c $(srcdir)/cubescan.c
