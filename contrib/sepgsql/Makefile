# contrib/sepgsql/Makefile

MODULE_big = sepgsql
OBJS = hooks.o selinux.o label.o dml.o \
	schema.o relation.o proc.o
DATA_built = sepgsql.sql
REGRESS = label dml misc
EXTRA_CLEAN = -r tmp *.pp sepgsql-regtest.if sepgsql-regtest.fc

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/sepgsql
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lselinux, $(LIBS))
REGRESS_OPTS += --launcher $(top_builddir)/contrib/sepgsql/launcher
