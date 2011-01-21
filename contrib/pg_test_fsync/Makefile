#
# Makefile for test_fsync
#
# contrib/test_fsync/Makefile

PGFILEDESC = "test_fsync - test various disk sync methods"
PGAPPICON = win32

PROGRAM  = test_fsync
OBJS = test_fsync.o

PG_LIBS = $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_fsync
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
