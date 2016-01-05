# contrib/hstore_plperl/Makefile

MODULE_big = hstore_plperl
OBJS = hstore_plperl.o $(WIN32RES)
PGFILEDESC = "hstore_plperl - hstore transform for plperl"

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plperl -I$(top_srcdir)/contrib/hstore

EXTENSION = hstore_plperl hstore_plperlu
DATA = hstore_plperl--1.0.sql hstore_plperlu--1.0.sql

REGRESS = hstore_plperl hstore_plperlu create_transform
EXTRA_INSTALL = contrib/hstore

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/hstore_plperl
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# In configurations that forbid undefined symbols in libraries, link with each
# dependency.  This does preclude pgxs builds.
ifeq ($(PORTNAME), aix)
rpathdir = $(pkglibdir):$(perl_archlibexp)/CORE
SHLIB_LINK += ../hstore/libhstore.exp $(perl_embed_ldflags)
endif
ifeq ($(PORTNAME), win32)
# these settings are the same as for plperl
override CPPFLAGS += -DPLPERL_HAVE_UID_GID -Wno-comment
SHLIB_LINK += ../hstore/libhstore.a $(sort $(wildcard ../../src/pl/plperl/libperl*.a))
endif

ifeq ($(PORTNAME), cygwin)
SHLIB_LINK += -L../hstore -l hstore $(perl_embed_ldflags)
endif

# As with plperl we need to make sure that the CORE directory is included
# last, probably because it sometimes contains some header files with names
# that clash with some of ours, or with some that we include, notably on
# Windows.
override CPPFLAGS := $(CPPFLAGS) -I$(perl_archlibexp)/CORE
