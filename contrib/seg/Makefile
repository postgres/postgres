# contrib/seg/Makefile

MODULE_big = seg
OBJS = \
	$(WIN32RES) \
	seg.o \
	segparse.o \
	segscan.o

EXTENSION = seg
DATA = seg--1.1.sql seg--1.1--1.2.sql seg--1.2--1.3.sql seg--1.3--1.4.sql \
	seg--1.0--1.1.sql
PGFILEDESC = "seg - line segment data type"

HEADERS = segdata.h

REGRESS = security seg partition

EXTRA_CLEAN = segparse.h segparse.c segscan.c

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


# See notes in src/backend/parser/Makefile about the following two rules
segparse.h: segparse.c
	touch $@

segparse.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
segparse.o segscan.o: segparse.h
