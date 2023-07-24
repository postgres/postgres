# contrib/pg_tde/Makefile

MODULE_big = pg_tde
#OBJS = \
        pg_tdeam.o \
        pg_tdeam_handler.o \
        pg_tdeam_visibility.o \
        pg_tdetoast.o \
        pg_tde_io.o \
        pg_tde_prune.o \
        pg_tde_rewrite.o \
        pg_tde_vacuumlazy.o \
        pg_tde_visibilitymap.o

OBJS = \
        pg_tdeam.o \
        pg_tdeam_handler.o \
        pg_tdeam_visibility.o \
        pg_tdetoast.o \
        pg_tde_io.o \
        pg_tde_prune.o \
        pg_tde_rewrite.o \
        pg_tde_vacuumlazy.o \
        pg_tde_visibilitymap.o

EXTENSION = pg_tde
DATA = pg_tde--1.0.sql
PGFILEDESC = "pg_tde access method"

REGRESS = pg_tde

TAP_TESTS = 0

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_tde
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
