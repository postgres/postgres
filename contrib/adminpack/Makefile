MODULE_big = adminpack
PG_CPPFLAGS = -I$(libpq_srcdir)
DATA_built = adminpack.sql
DATA = uninstall_adminpack.sql
DOCS = README.adminpack
OBJS = adminpack.o

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/adminpack
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
