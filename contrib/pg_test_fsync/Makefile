# contrib/pg_test_fsync/Makefile

PGFILEDESC = "pg_test_fsync - test various disk sync methods"
PGAPPICON = win32

PROGRAM  = pg_test_fsync
OBJS = pg_test_fsync.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_test_fsync
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
