# $PostgreSQL: pgsql/contrib/cube/Makefile,v 1.23 2009/08/28 20:26:18 petere Exp $

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
cubeparse.o: cubescan.c

cubeparse.c: cubeparse.y
ifdef BISON
	$(BISON) $(BISONFLAGS) -o $@ $<
else
	@$(missing) bison $< $@
endif

cubescan.c: cubescan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: cubeparse.c cubescan.c

maintainer-clean:
	rm -f cubeparse.c cubescan.c
