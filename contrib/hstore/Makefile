subdir = contrib/hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global


override CPPFLAGS := -I. $(CPPFLAGS)

MODULE_big = hstore
OBJS = hstore_io.o hstore_op.o hstore_gist.o crc32.o

DATA_built = hstore.sql
DATA = uninstall_hstore.sql
DOCS = README.hstore
REGRESS = hstore

include $(top_srcdir)/contrib/contrib-global.mk
# DO NOT DELETE

