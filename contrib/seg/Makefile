# contrib/seg/Makefile

MODULE_big = seg
OBJS = \
	$(WIN32RES) \
	seg.o \
	segparse.o

EXTENSION = seg
DATA = seg--1.1.sql seg--1.1--1.2.sql seg--1.2--1.3.sql seg--1.3--1.4.sql \
	seg--1.0--1.1.sql
PGFILEDESC = "seg - line segment data type"

HEADERS = segdata.h

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
