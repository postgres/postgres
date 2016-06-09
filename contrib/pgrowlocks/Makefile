# contrib/pgrowlocks/Makefile

MODULE_big	= pgrowlocks
OBJS		= pgrowlocks.o $(WIN32RES)

EXTENSION = pgrowlocks
DATA = pgrowlocks--1.2.sql pgrowlocks--1.1--1.2.sql pgrowlocks--1.0--1.1.sql \
	pgrowlocks--unpackaged--1.0.sql
PGFILEDESC = "pgrowlocks - display row locking information"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgrowlocks
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
