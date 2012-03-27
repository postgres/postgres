# contrib/pg_test_timing/Makefile

PGFILEDESC = "pg_test_timing - test timing overhead"
PGAPPICON = win32

PROGRAM  = pg_test_timing
OBJS = pg_test_timing.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_test_timing
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
