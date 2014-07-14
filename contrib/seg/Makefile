# contrib/seg/Makefile

MODULE_big = seg
OBJS = seg.o segparse.o $(WIN32RES)

EXTENSION = seg
DATA = seg--1.0.sql seg--unpackaged--1.0.sql
PGFILEDESC = "seg - line segment data type"

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

distprep: segparse.c segscan.c

maintainer-clean:
	rm -f segparse.c segscan.c
