# $PostgreSQL: pgsql/contrib/hstore/Makefile,v 1.7 2009/09/30 19:50:22 tgl Exp $

subdir = contrib/hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = hstore
OBJS = hstore_io.o hstore_op.o hstore_gist.o hstore_gin.o hstore_compat.o \
	crc32.o

DATA_built = hstore.sql
DATA = uninstall_hstore.sql
REGRESS = hstore

include $(top_srcdir)/contrib/contrib-global.mk
