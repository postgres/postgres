# src/test/modules/test_int128/Makefile

PGFILEDESC = "test_int128 - test 128-bit integer arithmetic"

PROGRAM = test_int128
OBJS = $(WIN32RES) test_int128.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS_INTERNAL += $(libpq_pgport)

NO_INSTALL = 1
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_int128
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
