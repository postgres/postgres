# $PostgreSQL: pgsql/contrib/intarray/Makefile,v 1.13 2006/02/27 12:54:39 petere Exp $

MODULE_big = _int
OBJS = _int_bool.o _int_gist.o _int_op.o _int_tool.o _intbig_gist.o 
DATA_built = _int.sql
DATA = uninstall__int.sql
DOCS = README.intarray
REGRESS = _int

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/intarray
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
