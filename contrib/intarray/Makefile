# contrib/intarray/Makefile

MODULE_big = _int
OBJS = _int_bool.o _int_gist.o _int_op.o _int_tool.o \
	_intbig_gist.o _int_gin.o _int_selfuncs.o $(WIN32RES)

EXTENSION = intarray
DATA = intarray--1.2.sql intarray--1.1--1.2.sql intarray--1.0--1.1.sql \
	intarray--unpackaged--1.0.sql
PGFILEDESC = "intarray - functions and operators for arrays of integers"

REGRESS = _int

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/intarray
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
