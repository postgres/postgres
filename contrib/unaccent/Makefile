# contrib/unaccent/Makefile

MODULE_big = unaccent
OBJS = \
	$(WIN32RES) \
	unaccent.o

EXTENSION = unaccent
DATA = unaccent--1.1.sql unaccent--1.0--1.1.sql
DATA_TSEARCH = unaccent.rules
PGFILEDESC = "unaccent - text search dictionary that removes accents"

REGRESS = unaccent

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/unaccent
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

update-unicode: $(srcdir)/unaccent.rules

# Allow running this even without --with-python
ifeq ($(PYTHON),)
PYTHON = python
endif

$(srcdir)/unaccent.rules: generate_unaccent_rules.py ../../src/common/unicode/UnicodeData.txt Latin-ASCII.xml
	$(PYTHON) $< --unicode-data-file $(word 2,$^) --latin-ascii-file $(word 3,$^) >$@

# Only download it once; dependencies must match src/common/unicode/
../../src/common/unicode/UnicodeData.txt: $(top_builddir)/src/Makefile.global
	$(MAKE) -C $(@D) $(@F)

# Dependency on Makefile.global is for CLDR_VERSION
Latin-ASCII.xml: $(top_builddir)/src/Makefile.global
	$(DOWNLOAD) https://raw.githubusercontent.com/unicode-org/cldr/release-$(subst .,-,$(CLDR_VERSION))/common/transforms/Latin-ASCII.xml

distclean:
	rm -f Latin-ASCII.xml
