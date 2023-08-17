# contrib/pg_tde/Makefile

MODULE_big = pg_tde

OBJS = \
        enc_tuple.o \
        enc_aes.o 

PG_CPPFLAGS = -I$(top_builddir)/$(subdir)/../

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/postgres-tde-ext/src/access
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


