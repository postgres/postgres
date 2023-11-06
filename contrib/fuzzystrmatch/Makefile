# contrib/fuzzystrmatch/Makefile

MODULE_big = fuzzystrmatch
OBJS = \
	$(WIN32RES) \
	daitch_mokotoff.o \
	dmetaphone.o \
	fuzzystrmatch.o

EXTENSION = fuzzystrmatch
DATA = fuzzystrmatch--1.1.sql fuzzystrmatch--1.1--1.2.sql \
	fuzzystrmatch--1.0--1.1.sql

PGFILEDESC = "fuzzystrmatch - similarities and distance between strings"

REGRESS = fuzzystrmatch fuzzystrmatch_utf8

EXTRA_CLEAN = daitch_mokotoff.h

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/fuzzystrmatch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# Force this dependency to be known even without dependency info built:
daitch_mokotoff.o: daitch_mokotoff.h

daitch_mokotoff.h: daitch_mokotoff_header.pl
	$(PERL) $< $@
