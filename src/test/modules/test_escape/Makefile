# src/test/modules/test_escape/Makefile

PGFILEDESC = "test escape program"
PGAPPICON = win32

PROGRAM = test_escape
OBJS = $(WIN32RES) test_escape.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

NO_INSTALL = 1
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_escape
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

test_escape$(X): | submake-libpgfeutils
check: test_escape$(X)
