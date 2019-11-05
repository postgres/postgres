# contrib/pg_standby/Makefile

PGFILEDESC = "pg_standby - supports creation of a warm standby"
PGAPPICON = win32

PROGRAM = pg_standby
OBJS = \
	$(WIN32RES) \
	pg_standby.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_standby
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
