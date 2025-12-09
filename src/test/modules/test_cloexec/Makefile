# src/test/modules/test_cloexec/Makefile

# This module is for Windows only
ifeq ($(PORTNAME),win32)

MODULE_big = test_cloexec
OBJS = \
	$(WIN32RES) \
	test_cloexec.o

PGFILEDESC = "test_cloexec - test O_CLOEXEC flag handling"

# Build as a standalone program, not a shared library
PROGRAM = test_cloexec
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

TAP_TESTS = 1

endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_cloexec
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
