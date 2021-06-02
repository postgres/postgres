# src/test/modules/libpq_pipeline/Makefile

PGFILEDESC = "libpq_pipeline - test program for pipeline execution"
PGAPPICON = win32

PROGRAM = libpq_pipeline
OBJS = $(WIN32RES) libpq_pipeline.o

NO_INSTALL = 1

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS_INTERNAL += $(libpq_pgport)

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/libpq_pipeline
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
