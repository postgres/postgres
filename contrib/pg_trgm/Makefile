subdir = contrib/pg_trgm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I. $(CPPFLAGS)

MODULE_big = pg_trgm
OBJS = trgm_op.o trgm_gist.o 

DATA_built = pg_trgm.sql
DOCS = README.pg_trgm
REGRESS = pg_trgm

include $(top_srcdir)/contrib/contrib-global.mk
# DO NOT DELETE

