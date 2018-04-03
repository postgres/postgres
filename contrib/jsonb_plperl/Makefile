# contrib/jsonb_plperl/Makefile

MODULE_big = jsonb_plperl
OBJS = jsonb_plperl.o $(WIN32RES)
PGFILEDESC = "jsonb_plperl - jsonb transform for plperl"

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plperl

EXTENSION = jsonb_plperlu jsonb_plperl
DATA = jsonb_plperlu--1.0.sql jsonb_plperl--1.0.sql

REGRESS = jsonb_plperl jsonb_plperlu

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/jsonb_plperl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# We must link libperl explicitly
ifeq ($(PORTNAME), win32)
# these settings are the same as for plperl
override CPPFLAGS += -DPLPERL_HAVE_UID_GID -Wno-comment
# ... see silliness in plperl Makefile ...
SHLIB_LINK += $(sort $(wildcard ../../src/pl/plperl/libperl*.a))
else
rpathdir = $(perl_archlibexp)/CORE
SHLIB_LINK += $(perl_embed_ldflags)
endif

# As with plperl we need to make sure that the CORE directory is included
# last, probably because it sometimes contains some header files with names
# that clash with some of ours, or with some that we include, notably on
# Windows.
override CPPFLAGS := $(CPPFLAGS) $(perl_embed_ccflags) -I$(perl_archlibexp)/CORE
