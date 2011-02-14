# contrib/spi/Makefile

MODULES = autoinc insert_username moddatetime refint timetravel

EXTENSION = autoinc insert_username moddatetime refint timetravel

DATA = autoinc--1.0.sql autoinc--unpackaged--1.0.sql \
       insert_username--1.0.sql insert_username--unpackaged--1.0.sql \
       moddatetime--1.0.sql moddatetime--unpackaged--1.0.sql \
       refint--1.0.sql refint--unpackaged--1.0.sql \
       timetravel--1.0.sql timetravel--unpackaged--1.0.sql

DOCS = $(addsuffix .example, $(MODULES))

# this is needed for the regression tests;
# comment out if you want a quieter refint package for other uses
PG_CPPFLAGS = -DREFINT_VERBOSE

LDFLAGS_SL += -L$(top_builddir)/src/port -lpgport

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/spi
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
