# contrib/jsonb_plpython/Makefile

MODULE_big = jsonb_plpython$(python_majorversion)
OBJS = \
	$(WIN32RES) \
	jsonb_plpython.o
PGFILEDESC = "jsonb_plpython - transform between jsonb and plpythonu"

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plpython $(python_includespec) -DPLPYTHON_LIBNAME='"plpython$(python_majorversion)"'

EXTENSION = jsonb_plpythonu jsonb_plpython2u jsonb_plpython3u
DATA = jsonb_plpythonu--1.0.sql jsonb_plpython2u--1.0.sql jsonb_plpython3u--1.0.sql

REGRESS = jsonb_plpython
REGRESS_PLPYTHON3_MANGLE := $(REGRESS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/jsonb_plpython
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# We must link libpython explicitly
ifeq ($(PORTNAME), win32)
# ... see silliness in plpython Makefile ...
SHLIB_LINK_INTERNAL += $(sort $(wildcard ../../src/pl/plpython/libpython*.a))
else
rpathdir = $(python_libdir)
SHLIB_LINK += $(python_libspec) $(python_additional_libs)
endif

ifeq ($(python_majorversion),2)
REGRESS_OPTS += --load-extension=plpythonu --load-extension=jsonb_plpythonu
endif

include $(top_srcdir)/src/pl/plpython/regress-python3-mangle.mk
