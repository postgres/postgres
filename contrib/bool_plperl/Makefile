# contrib/bool_plperl/Makefile

MODULE_big = bool_plperl
OBJS = \
	$(WIN32RES) \
	bool_plperl.o
PGFILEDESC = "bool_plperl - bool transform for plperl"

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plperl

EXTENSION = bool_plperlu bool_plperl
DATA = bool_plperlu--1.0.sql bool_plperl--1.0.sql

REGRESS = bool_plperl bool_plperlu

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/bool_plperl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# We must link libperl explicitly
ifeq ($(PORTNAME), win32)
# these settings are the same as for plperl
override CPPFLAGS += -DPLPERL_HAVE_UID_GID -Wno-comment
# ... see silliness in plperl Makefile ...
SHLIB_LINK_INTERNAL += $(sort $(wildcard ../../src/pl/plperl/libperl*.a))
else
rpathdir = $(perl_archlibexp)/CORE
SHLIB_LINK += $(perl_embed_ldflags)
endif

# As with plperl we need to include the perl_includespec directory last.
override CPPFLAGS := $(CPPFLAGS) $(perl_embed_ccflags) $(perl_includespec)
