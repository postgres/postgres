# contrib/pg_overexplain/Makefile

MODULE_big = pg_overexplain
OBJS = \
	$(WIN32RES) \
	pg_overexplain.o

PGFILEDESC = "pg_overexplain - allow EXPLAIN to dump even more details"

REGRESS = pg_overexplain

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_overexplain
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
