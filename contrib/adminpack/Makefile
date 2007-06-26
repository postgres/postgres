# $PostgreSQL: pgsql/contrib/adminpack/Makefile,v 1.5 2007/06/26 22:05:01 tgl Exp $

MODULE_big = adminpack
PG_CPPFLAGS = -I$(libpq_srcdir)
DATA_built = adminpack.sql
DATA = uninstall_adminpack.sql
DOCS = README.adminpack
OBJS = adminpack.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/adminpack
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
