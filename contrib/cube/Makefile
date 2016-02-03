# contrib/cube/Makefile

MODULE_big = cube
OBJS= cube.o cubeparse.o $(WIN32RES)

EXTENSION = cube
DATA = cube--1.1.sql cube--1.0--1.1.sql cube--unpackaged--1.0.sql
PGFILEDESC = "cube - multidimensional cube data type"

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

distprep: cubeparse.c cubescan.c

maintainer-clean:
	rm -f cubeparse.c cubescan.c
