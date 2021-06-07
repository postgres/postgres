# contrib/zenith/Makefile


MODULE_big = zenith
OBJS = \
	$(WIN32RES) \
	inmem_smgr.o libpagestore.o pagestore_smgr.o

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)

EXTENSION = zenith
PGFILEDESC = "zenith - cloud storage for PostgreSQL"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
SHLIB_PREREQS = submake-libpq
subdir = contrib/zenith
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
