# $PostgreSQL: pgsql/contrib/cube/Makefile,v 1.18 2006/04/03 18:47:41 petere Exp $

MODULE_big = cube
OBJS= cube.o cubeparse.o

DATA_built = cube.sql
DATA = uninstall_cube.sql
DOCS = README.cube
REGRESS = cube

EXTRA_CLEAN = y.tab.c y.tab.h

PG_CPPFLAGS = -I.

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/cube
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


# cubescan is compiled as part of cubeparse
cubeparse.o: $(srcdir)/cubescan.c

# See notes in src/backend/parser/Makefile about the following two rules

$(srcdir)/cubeparse.c: $(srcdir)/cubeparse.h ;

$(srcdir)/cubeparse.h: cubeparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) $<
	mv -f y.tab.c $(srcdir)/cubeparse.c
	mv -f y.tab.h $(srcdir)/cubeparse.h
else
	@$(missing) bison $< $@
endif

$(srcdir)/cubescan.c: cubescan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: $(srcdir)/cubeparse.c $(srcdir)/cubeparse.h $(srcdir)/cubescan.c

maintainer-clean:
	rm -f $(srcdir)/cubeparse.c $(srcdir)/cubeparse.h $(srcdir)/cubescan.c
