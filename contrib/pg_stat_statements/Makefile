# contrib/pg_stat_statements/Makefile

MODULE_big = pg_stat_statements
OBJS = \
	$(WIN32RES) \
	pg_stat_statements.o

EXTENSION = pg_stat_statements
DATA = pg_stat_statements--1.4.sql \
	pg_stat_statements--1.11--1.12.sql pg_stat_statements--1.10--1.11.sql \
	pg_stat_statements--1.9--1.10.sql pg_stat_statements--1.8--1.9.sql \
	pg_stat_statements--1.7--1.8.sql pg_stat_statements--1.6--1.7.sql \
	pg_stat_statements--1.5--1.6.sql pg_stat_statements--1.4--1.5.sql \
	pg_stat_statements--1.3--1.4.sql pg_stat_statements--1.2--1.3.sql \
	pg_stat_statements--1.1--1.2.sql pg_stat_statements--1.0--1.1.sql
PGFILEDESC = "pg_stat_statements - execution statistics of SQL statements"

LDFLAGS_SL += $(filter -lm, $(LIBS))

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/pg_stat_statements/pg_stat_statements.conf
REGRESS = select dml cursors utility level_tracking planning \
	user_activity wal entry_timestamp privileges extended \
	parallel cleanup oldextversions squashing
# Disabled because these tests require "shared_preload_libraries=pg_stat_statements",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_statements
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
