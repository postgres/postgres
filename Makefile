# contrib/pg_tde/Makefile

PGFILEDESC = "pg_tde access method"
MODULE_big = pg_tde
EXTENSION = pg_tde
DATA = pg_tde--1.0.sql
REGRESS = pg_tde
TAP_TESTS = 0

OBJS = src/encryption/enc_tuple.o \
src/encryption/enc_aes.o \
src/access/pg_tde_io.o \
src/access/pg_tdeam_visibility.o \
src/access/pg_tde_tdemap.o \
src/access/pg_tdeam.o \
src/access/pg_tdetoast.o \
src/access/pg_tde_prune.o \
src/access/pg_tde_vacuumlazy.o \
src/access/pg_tde_visibilitymap.o \
src/access/pg_tde_rewrite.o \
src/access/pg_tdeam_handler.o \
src/transam/pg_tde_xact_handler.o \
src/pg_tde.o


ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
override PG_CPPFLAGS += -I$(CURDIR)/src/include
include $(PGXS)
else
subdir = contrib/postgres-tde-ext
top_builddir = ../..
override PG_CPPFLAGS += -I$(top_srcdir)/$(subdir)/src/include
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lcrypto -lssl, $(LIBS))
