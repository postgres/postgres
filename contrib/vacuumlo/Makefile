# contrib/vacuumlo/Makefile

PGFILEDESC = "vacuumlo - removes orphaned large objects"
PGAPPICON = win32

PROGRAM = vacuumlo
OBJS = \
	$(WIN32RES) \
	vacuumlo.o

TAP_TESTS = 1

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS_INTERNAL = $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
