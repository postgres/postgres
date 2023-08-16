# src/test/modules/test_oat_hooks/Makefile

MODULE_big = test_oat_hooks
OBJS = \
	$(WIN32RES) \
	test_oat_hooks.o
PGFILEDESC = "test_oat_hooks - example use of object access hooks"

REGRESS = test_oat_hooks alter_table

# disable installcheck for now
NO_INSTALLCHECK = 1
# and also for now force NO_LOCALE and UTF8
ENCODING = UTF8
NO_LOCALE = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_oat_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
