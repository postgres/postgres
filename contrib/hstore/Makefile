# $PostgreSQL: pgsql/contrib/hstore/Makefile,v 1.6 2007/11/10 23:59:51 momjian Exp $

subdir = contrib/hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = hstore
OBJS = hstore_io.o hstore_op.o hstore_gist.o hstore_gin.o crc32.o

DATA_built = hstore.sql
DATA = uninstall_hstore.sql
REGRESS = hstore

include $(top_srcdir)/contrib/contrib-global.mk
