
subdir = contrib/btree_gist
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = btree_gist
OBJS= btree_common.o btree_int2.o btree_int4.o btree_int8.o btree_float4.o btree_float8.o btree_ts.o
DATA_built = btree_gist.sql
DOCS = README.btree_gist
REGRESS = btree_gist

EXTRA_CLEAN = btree_int2.c btree_int4.c btree_int8.c btree_float4.c btree_float8.c 

include $(top_srcdir)/contrib/contrib-global.mk

btree_int2.c: btree_num.c.in
	sed 's,__DEFINE_BTREE_TYPE_HERE__,BTREE_GIST_INT2,g;s,__BTREE_GIST_TYPE__,int16,g;s,__BTREE_GIST_TYPE2__,int2,g' < $<  > $@

btree_int4.c: btree_num.c.in
	sed 's,__DEFINE_BTREE_TYPE_HERE__,BTREE_GIST_INT4,g;s,__BTREE_GIST_TYPE__,int32,g;s,__BTREE_GIST_TYPE2__,int4,g' < $<  > $@

btree_int8.c: btree_num.c.in
	sed 's,__DEFINE_BTREE_TYPE_HERE__,BTREE_GIST_INT8,g;s,__BTREE_GIST_TYPE__,int64,g;s,__BTREE_GIST_TYPE2__,int8,g' < $<  > $@

btree_float4.c: btree_num.c.in
	sed 's,__DEFINE_BTREE_TYPE_HERE__,BTREE_GIST_FLOAT4,g;s,__BTREE_GIST_TYPE__,float4,g;s,__BTREE_GIST_TYPE2__,float4,g' < $<  > $@

btree_float8.c: btree_num.c.in
	sed 's,__DEFINE_BTREE_TYPE_HERE__,BTREE_GIST_FLOAT8,g;s,__BTREE_GIST_TYPE__,float8,g;s,__BTREE_GIST_TYPE2__,float8,g' < $<  > $@
