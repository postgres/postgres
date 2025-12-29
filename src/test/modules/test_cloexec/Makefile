# src/test/modules/test_cloexec/Makefile

PGFILEDESC = "test_cloexec - test O_CLOEXEC flag handling"
PGAPPICON = win32

PROGRAM = test_cloexec
OBJS = $(WIN32RES) test_cloexec.o

ifeq ($(PORTNAME), win32)
# Force Mkvcbuild.pm to link libpgport
PG_LIBS_INTERNAL = $(libpq_pgport)
endif

NO_INSTALLCHECK = 1
TAP_TESTS = 1

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
