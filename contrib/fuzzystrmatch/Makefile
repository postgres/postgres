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

# daitch_mokotoff.h is included in tarballs, so it has to be made by
# "distprep" and not cleaned except by "maintainer-clean".
distprep: daitch_mokotoff.h

maintainer-clean:
	rm -f daitch_mokotoff.h
