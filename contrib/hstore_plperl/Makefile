# contrib/hstore_plperl/Makefile

MODULE_big = hstore_plperl
OBJS = hstore_plperl.o

PG_CPPFLAGS = -I$(top_srcdir)/src/pl/plperl -I$(top_srcdir)/contrib/hstore -I$(perl_archlibexp)/CORE

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

# see plperl
ifeq ($(PORTNAME), win32)
override CPPFLAGS += -DPLPERL_HAVE_UID_GID -Wno-comment
endif
