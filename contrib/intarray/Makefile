# $PostgreSQL: pgsql/contrib/intarray/Makefile,v 1.16 2007/11/10 23:59:51 momjian Exp $

MODULE_big = _int
OBJS = _int_bool.o _int_gist.o _int_op.o _int_tool.o _intbig_gist.o _int_gin.o 
DATA_built = _int.sql
DATA = uninstall__int.sql
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
