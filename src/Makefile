# contrib/pg_tde/Makefile

MODULE_big = pg_tde

tde_subdirs = access encryption

OBJS = \
	$(patsubst %.c,%.o,$(foreach dir,$(tde_subdirs), $(wildcard $(dir)/*.c)))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/postgres-tde-ext/src
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

$(recurse)
