# contrib/fuzzystrmatch/Makefile

MODULE_big = fuzzystrmatch
OBJS = fuzzystrmatch.o dmetaphone.o
DATA_built = fuzzystrmatch.sql
DATA = uninstall_fuzzystrmatch.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/fuzzystrmatch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
