# contrib/pg_plan_advice/Makefile

MODULE_big = pg_plan_advice
OBJS = \
	$(WIN32RES) \
	pg_plan_advice.o \
	pgpa_ast.o \
	pgpa_identifier.o \
	pgpa_join.o \
	pgpa_output.o \
	pgpa_parser.o \
	pgpa_planner.o \
	pgpa_scan.o \
	pgpa_scanner.o \
	pgpa_trove.o \
	pgpa_walker.o

PGFILEDESC = "pg_plan_advice - help the planner get the right plan"

REGRESS = gather join_order join_strategy partitionwise prepared \
	scan semijoin syntax

EXTRA_CLEAN = pgpa_parser.h pgpa_parser.c pgpa_scanner.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_plan_advice
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# See notes in src/backend/parser/Makefile about the following two rules
pgpa_parser.h: pgpa_parser.c
	touch $@

pgpa_parser.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
pgpa_parser.o pgpa_scanner.o: pgpa_parser.h
