# $Header: /cvsroot/pgsql/contrib/intarray/Makefile,v 1.9 2003/06/11 18:44:14 momjian Exp $

subdir = contrib/intarray
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = _int
OBJS = _int_bool.o _int_gist.o _int_op.o _int_tool.o _intbig_gist.o 
DATA_built = _int.sql
DOCS = README.intarray
REGRESS = _int

include $(top_srcdir)/contrib/contrib-global.mk
