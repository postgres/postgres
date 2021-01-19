# contrib/pageinspect/Makefile

MODULE_big	= pageinspect
OBJS = \
	$(WIN32RES) \
	brinfuncs.o \
	btreefuncs.o \
	fsmfuncs.o \
	ginfuncs.o \
	gistfuncs.o \
	hashfuncs.o \
	heapfuncs.o \
	rawpage.o

EXTENSION = pageinspect
DATA =  pageinspect--1.8--1.9.sql \
	pageinspect--1.7--1.8.sql pageinspect--1.6--1.7.sql \
	pageinspect--1.5.sql pageinspect--1.5--1.6.sql \
	pageinspect--1.4--1.5.sql pageinspect--1.3--1.4.sql \
	pageinspect--1.2--1.3.sql pageinspect--1.1--1.2.sql \
	pageinspect--1.0--1.1.sql
PGFILEDESC = "pageinspect - functions to inspect contents of database pages"

REGRESS = page btree brin gin gist hash checksum oldextversions

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pageinspect
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
