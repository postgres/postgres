# src/test/modules/test_plan_advice/Makefile

PGFILEDESC = "test_plan_advice - test whether generated plan advice works"

MODULE_big = test_plan_advice
OBJS = \
	$(WIN32RES) \
	test_plan_advice.o

EXTRA_INSTALL = contrib/pg_plan_advice

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
override CPPFLAGS += -I$(includedir_server)/contrib/pg_plan_advice
else
subdir = src/test/modules/test_plan_advice
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
override CPPFLAGS += -I$(top_srcdir)/contrib/pg_plan_advice
endif

REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB
