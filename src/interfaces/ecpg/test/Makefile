# src/interfaces/ecpg/test/Makefile

PGFILEDESC = "ECPG Test - regression tests for ECPG"
PGAPPICON = win32

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := \
	'-I$(top_builddir)/src/port' \
	'-I$(top_srcdir)/src/test/regress' \
	'-I$(libpq_srcdir)' \
	'-DHOST_TUPLE="$(host_tuple)"' \
	'-DSHELLPROG="$(SHELL)"' \
	$(CPPFLAGS)

ifneq ($(build_os),mingw32)
abs_builddir := $(shell pwd)
else
abs_builddir := $(shell sh -c "pwd -W")
endif

all install installdirs uninstall:
	$(MAKE) -C connect $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C preproc $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C compat_oracle $@
	$(MAKE) -C thread $@

clean distclean:
	$(MAKE) -C connect $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C preproc $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C compat_oracle $@
	$(MAKE) -C thread $@
	rm -rf tmp_check results log
	rm -f pg_regress regression.diffs regression.out pg_regress_ecpg.o $(WIN32RES)

# Build regression test driver

all: pg_regress$(X)

pg_regress$(X): pg_regress_ecpg.o $(WIN32RES) $(top_builddir)/src/test/regress/pg_regress.o
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@

$(top_builddir)/src/test/regress/pg_regress.o:
	$(MAKE) -C $(dir $@) $(notdir $@)

# dependencies ensure that path changes propagate
pg_regress_ecpg.o: pg_regress_ecpg.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h: $(top_builddir)/src/Makefile.global
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

# Common options for tests
#
# Need to specify expecteddir explicitly, as the inputdir is located in the
# build directory, because the files need to be compiled. Other pg_regress
# style tests have the expecteddir in the source directory.
#
# Also pick up anything passed in EXTRA_REGRESS_OPTS.
REGRESS_OPTS =  --expecteddir=$(srcdir) \
  --dbname=ecpg1_regression,ecpg2_regression --create-role=regress_ecpg_user1,regress_ecpg_user2 \
  $(EXTRA_REGRESS_OPTS)

check: all
	$(with_temp_install) ./pg_regress $(REGRESS_OPTS) --temp-instance=./tmp_check $(TEMP_CONF) --bindir= $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule sql/twophase

# Connect to the server using TCP, and add a TCP-specific test.
checktcp: all | temp-install
	$(with_temp_install) ./pg_regress $(REGRESS_OPTS) --temp-instance=./tmp_check $(TEMP_CONF) --bindir= $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule --host=localhost sql/twophase connect/test1

installcheck: all
	./pg_regress $(REGRESS_OPTS) --bindir='$(bindir)' $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule

# Versions of the check tests that include the twophase commit test.
# It only makes sense to run these if set up to use prepared transactions,
# via TEMP_CONFIG for the check case, or via the postgresql.conf for the
# installcheck case.

installcheck-prepared-txns: all
	./pg_regress $(REGRESS_OPTS) --bindir='$(bindir)' $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule sql/twophase

check-prepared-txns: all | temp-install
	$(with_temp_install) ./pg_regress $(REGRESS_OPTS) --temp-instance=./tmp_check $(TEMP_CONF) --bindir= $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule sql/twophase
