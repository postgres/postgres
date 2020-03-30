# contrib/pg_trgm/Makefile

MODULE_big = pg_trgm
OBJS = \
	$(WIN32RES) \
	trgm_gin.o \
	trgm_gist.o \
	trgm_op.o \
	trgm_regexp.o

EXTENSION = pg_trgm
DATA = pg_trgm--1.4--1.5.sql pg_trgm--1.3--1.4.sql \
	pg_trgm--1.3.sql pg_trgm--1.2--1.3.sql pg_trgm--1.1--1.2.sql \
	pg_trgm--1.0--1.1.sql
PGFILEDESC = "pg_trgm - trigram matching"

REGRESS = pg_trgm pg_word_trgm pg_strict_word_trgm

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_trgm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
