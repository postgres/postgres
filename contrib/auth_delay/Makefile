# contrib/auth_delay/Makefile

MODULES = auth_delay
PGFILEDESC = "auth_delay - delay authentication failure reports"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/auth_delay
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
