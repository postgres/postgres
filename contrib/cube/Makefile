# $Header: /cvsroot/pgsql/contrib/cube/Makefile,v 1.8 2003/01/31 20:58:00 tgl Exp $

subdir = contrib/cube
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = cube
OBJS= cube.o cubeparse.o buffer.o

DATA_built = cube.sql
DOCS = README.cube
REGRESS = cube


# cubescan is compiled as part of cubeparse
cubeparse.o: cubescan.c

cubeparse.c: cubeparse.h ;

# The sed hack is so that we can get the same error messages with
# bison 1.875 and later as we did with earlier bisons.  Eventually,
# I suppose, we should re-standardize on "syntax error" --- in which
# case flip the sed translation, but don't remove it.

cubeparse.h: cubeparse.y
ifdef YACC
	$(YACC) -d $(YFLAGS) -p cube_yy $<
	sed -e 's/"syntax error/"parse error/' < y.tab.c > cubeparse.c
	mv -f y.tab.h cubeparse.h
	rm -f y.tab.c
else
	@$(missing) bison $< $@
endif

cubescan.c: cubescan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -Pcube_yy -o'$@' $<
else
	@$(missing) flex $< $@
endif

EXTRA_CLEAN = cubeparse.c cubeparse.h cubescan.c y.tab.c y.tab.h


include $(top_srcdir)/contrib/contrib-global.mk
