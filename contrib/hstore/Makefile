# $PostgreSQL: pgsql/contrib/hstore/Makefile,v 1.5 2007/03/14 14:21:52 teodor Exp $

subdir = contrib/hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = hstore
OBJS = hstore_io.o hstore_op.o hstore_gist.o hstore_gin.o crc32.o

DATA_built = hstore.sql
DATA = uninstall_hstore.sql
DOCS = README.hstore
REGRESS = hstore

include $(top_srcdir)/contrib/contrib-global.mk
