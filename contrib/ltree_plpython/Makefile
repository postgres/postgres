# contrib/ltree_plpython/Makefile

MODULE_big = ltree_plpython$(python_majorversion)
OBJS = ltree_plpython.o $(WIN32RES)
PGFILEDESC = "ltree_plpython - ltree transform for plpython"

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plpython $(python_includespec) -I$(top_srcdir)/contrib/ltree

EXTENSION = ltree_plpythonu ltree_plpython2u ltree_plpython3u
DATA = ltree_plpythonu--1.0.sql ltree_plpython2u--1.0.sql ltree_plpython3u--1.0.sql

REGRESS = ltree_plpython
REGRESS_PLPYTHON3_MANGLE := $(REGRESS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/ltree_plpython
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# In configurations that forbid undefined symbols in libraries, link with each
# dependency.  This does preclude pgxs builds.
ifeq ($(PORTNAME), aix)
rpathdir = $(pkglibdir):$(python_libdir)
SHLIB_LINK += $(python_libspec) $(python_additional_libs) $(sort $(wildcard ../../src/pl/plpython/libplpython*.exp))
endif
ifeq ($(PORTNAME), win32)
SHLIB_LINK += $(sort $(wildcard ../../src/pl/plpython/libpython*.a)) $(sort $(wildcard ../../src/pl/plpython/libplpython*.a))
endif

ifeq ($(PORTNAME), cygwin)
SHLIB_LINK += -L../ltree -lltree -L../../src/pl/plpython \
	-lplpython$(python_majorversion) $(python_libspec)
endif

REGRESS_OPTS += --load-extension=ltree
ifeq ($(python_majorversion),2)
REGRESS_OPTS += --load-extension=plpythonu --load-extension=ltree_plpythonu
endif
EXTRA_INSTALL += contrib/ltree

include $(top_srcdir)/src/pl/plpython/regress-python3-mangle.mk
