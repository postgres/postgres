# contrib/btree_gist/Makefile

MODULE_big = btree_gist

OBJS =  btree_gist.o btree_utils_num.o btree_utils_var.o btree_int2.o \
        btree_int4.o btree_int8.o btree_float4.o btree_float8.o btree_cash.o \
        btree_oid.o btree_ts.o btree_time.o btree_date.o btree_interval.o \
        btree_macaddr.o btree_inet.o btree_text.o btree_bytea.o btree_bit.o \
        btree_numeric.o $(WIN32RES)

EXTENSION = btree_gist
DATA = btree_gist--1.2.sql btree_gist--1.1--1.2.sql btree_gist--1.0--1.1.sql \
	btree_gist--unpackaged--1.0.sql
PGFILEDESC = "btree_gist - B-tree equivalent GiST operator classes"

REGRESS = init int2 int4 int8 float4 float8 cash oid timestamp timestamptz \
        time timetz date interval macaddr inet cidr text varchar char bytea \
        bit varbit numeric not_equal

SHLIB_LINK += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/btree_gist
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
