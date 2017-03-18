# src/interfaces/ecpg/test/Makefile

PGFILEDESC = "ECPG Test - regression tests for ECPG"
PGAPPICON = win32

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := \
	'-I$(top_builddir)/src/port' \
	'-I$(top_srcdir)/src/test/regress' \
	'-DHOST_TUPLE="$(host_tuple)"' \
	'-DSHELLPROG="$(SHELL)"' \
	'-DDLSUFFIX="$(DLSUFFIX)"' \
	$(CPPFLAGS)

# default encoding for regression tests
ENCODING = SQL_ASCII

ifneq ($(build_os),mingw32)
abs_builddir := $(shell pwd)
else
abs_builddir := $(shell sh -c "pwd -W")
endif

all install installdirs uninstall distprep:
	$(MAKE) -C connect $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C preproc $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C thread $@

clean distclean maintainer-clean:
	$(MAKE) -C connect $@
	$(MAKE) -C sql $@
	$(MAKE) -C pgtypeslib $@
	$(MAKE) -C preproc $@
	$(MAKE) -C compat_informix $@
	$(MAKE) -C thread $@
	rm -rf tmp_check results log
	rm -f pg_regress regression.diffs regression.out pg_regress_ecpg.o $(WIN32RES)

# Build regression test driver

all: pg_regress$(X)

pg_regress$(X): pg_regress_ecpg.o $(WIN32RES) $(top_builddir)/src/test/regress/pg_regress.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(LDFLAGS_EX) $^ $(LIBS) -o $@

$(top_builddir)/src/test/regress/pg_regress.o:
	$(MAKE) -C $(dir $@) $(notdir $@)

# dependencies ensure that path changes propagate
pg_regress_ecpg.o: pg_regress_ecpg.c $(top_builddir)/src/port/pg_config_paths.h

$(top_builddir)/src/port/pg_config_paths.h: $(top_builddir)/src/Makefile.global
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

# When doing a VPATH build, copy over the .pgc, .stdout and .stderr
# files so that the driver script can find them.  We have to use an
# absolute path for the targets, because otherwise make will try to
# locate the missing files using VPATH, and will find them in
# $(srcdir), but the point here is that we want to copy them from
# $(srcdir) to the build directory.

ifdef VPATH
remaining_files_src := $(wildcard $(srcdir)/*/*.pgc) $(wildcard $(srcdir)/expected/*.c) $(wildcard $(srcdir)/expected/*.stdout) $(wildcard $(srcdir)/expected/*.stderr) $(srcdir)/resultmap
remaining_files_build := $(patsubst $(srcdir)/%, $(abs_builddir)/%, $(remaining_files_src))

all: $(remaining_files_build)
$(remaining_files_build): $(abs_builddir)/%: $(srcdir)/%
	ln -s $< $@
endif

# Common options for tests. Also pick up anything passed in EXTRA_REGRESS_OPTS
REGRESS_OPTS = --dbname=ecpg1_regression,ecpg2_regression --create-role=regress_ecpg_user1,regress_ecpg_user2 $(EXTRA_REGRESS_OPTS)

check: all
	$(with_temp_install) ./pg_regress $(REGRESS_OPTS) --temp-instance=./tmp_check $(TEMP_CONF) --bindir= $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule sql/twophase

# the same options, but with --listen-on-tcp
checktcp: all
	$(with_temp_install) ./pg_regress $(REGRESS_OPTS) --temp-instance=./tmp_check $(TEMP_CONF) --bindir= $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule_tcp --host=localhost

installcheck: all
	./pg_regress $(REGRESS_OPTS) --bindir='$(bindir)' $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule

# Versions of the check tests that include the twophase commit test.
# It only makes sense to run these if set up to use prepared transactions,
# via TEMP_CONFIG for the check case, or via the postgresql.conf for the
# installcheck case.

installcheck-prepared-txns: all
	./pg_regress $(REGRESS_OPTS) --bindir='$(bindir)' $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule sql/twophase

check-prepared-txns: all
	$(with_temp_install) ./pg_regress $(REGRESS_OPTS) --temp-instance=./tmp_check $(TEMP_CONF) --bindir= $(pg_regress_locale_flags) $(THREAD) --schedule=$(srcdir)/ecpg_schedule sql/twophase
