#-------------------------------------------------------------------------
#
# GNUmakefile--
#    Makefile for src/test/regress (the regression tests)
#
# Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/test/regress/GNUmakefile,v 1.62 2006/07/21 00:24:04 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/test/regress
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

contribdir := $(top_builddir)/contrib

# port number for temp-installation test postmaster
TEMP_PORT = 5$(DEF_PGPORT)

# where to find psql for testing an existing installation
PSQLDIR = $(bindir)

# default encoding
MULTIBYTE = SQL_ASCII

# maximum simultaneous connections for parallel tests
MAXCONNOPT :=
ifdef MAX_CONNECTIONS
MAXCONNOPT += --max-connections=$(MAX_CONNECTIONS)
endif

# locale
NOLOCALE :=
ifdef NO_LOCALE
NOLOCALE += --no-locale
endif

# stuff to pass into build of pg_regress
EXTRADEFS = '-DHOST_TUPLE="$(host_tuple)"' \
	'-DMAKEPROG="$(MAKE)"' \
	'-DSHELLPROG="$(SHELL)"'

##
## Prepare for tests
##

# Build regression test driver

all: submake-libpgport pg_regress$(X)

pg_regress$(X): pg_regress.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@

# dependencies ensure that path changes propagate
pg_regress.o: pg_regress.c $(top_builddir)/src/port/pg_config_paths.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(top_builddir)/src/port $(EXTRADEFS) -c -o $@ $<

$(top_builddir)/src/port/pg_config_paths.h: $(top_builddir)/src/Makefile.global
	$(MAKE) -C $(top_builddir)/src/port pg_config_paths.h

install: pg_regress$(X)
	$(INSTALL_PROGRAM) pg_regress$(X) '$(DESTDIR)$(pgxsdir)/$(subdir)/pg_regress$(X)'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/pg_regress$(X)'


# Build dynamically-loaded object file for CREATE FUNCTION ... LANGUAGE C.

NAME = regress
SO_MAJOR_VERSION= 0
SO_MINOR_VERSION= 0
OBJS = regress.o
SHLIB_LINK = $(BE_DLLLIBS)

include $(top_srcdir)/src/Makefile.shlib

all: $(NAME)$(DLSUFFIX)

$(NAME)$(DLSUFFIX): $(shlib)
	rm -f $(NAME)$(DLSUFFIX)
	$(LN_S) $(shlib) $(NAME)$(DLSUFFIX)

# Build test input and expected files

file_list := copy create_function_1 create_function_2 misc constraints tablespace
input_files  := $(foreach file, $(file_list), sql/$(file).sql)
output_files := $(foreach file, $(file_list), expected/$(file).out)

all: $(input_files) $(output_files)

ifneq ($(PORTNAME),win32)
abs_srcdir := $(shell cd $(srcdir) && pwd)
abs_builddir := $(shell pwd)
else
abs_srcdir := $(shell cd $(srcdir) && pwd -W)
abs_builddir := $(shell pwd -W)
endif

testtablespace := $(abs_builddir)/testtablespace


define sed-command
sed -e 's,@abs_srcdir@,$(abs_srcdir),g' \
    -e 's,@abs_builddir@,$(abs_builddir),g' \
    -e 's,@testtablespace@,$(testtablespace),g' \
    -e 's/@DLSUFFIX@/$(DLSUFFIX)/g' $< >$@
endef

$(input_files): sql/%.sql: input/%.source
	$(sed-command)

$(output_files): expected/%.out: output/%.source
	$(sed-command)

# When doing a VPATH build, copy over the remaining .sql and .out
# files so that the driver script can find them.  We have to use an
# absolute path for the targets, because otherwise make will try to
# locate the missing files using VPATH, and will find them in
# $(srcdir), but the point here is that we want to copy them from
# $(srcdir) to the build directory.

ifdef VPATH
remaining_files_src := $(wildcard $(srcdir)/sql/*.sql) $(wildcard $(srcdir)/expected/*.out) $(srcdir)/resultmap
remaining_files_build := $(patsubst $(srcdir)/%, $(abs_builddir)/%, $(remaining_files_src))

all: $(remaining_files_build)
$(remaining_files_build): $(abs_builddir)/%: $(srcdir)/%
	ln -s $< $@
endif


# And finally some extra C modules...

all: all-spi

.PHONY: all-spi
all-spi:
	$(MAKE) -C $(contribdir)/spi refint$(DLSUFFIX) autoinc$(DLSUFFIX)


##
## Run tests
##

check: all
	-rm -rf ./testtablespace
	mkdir ./testtablespace
	./pg_regress --temp-install=./tmp_check --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(MAXCONNOPT) $(NOLOCALE)

installcheck: all
	-rm -rf ./testtablespace
	mkdir ./testtablespace
	./pg_regress --psqldir=$(PSQLDIR) --schedule=$(srcdir)/serial_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE)

installcheck-parallel: all
	-rm -rf ./testtablespace
	mkdir ./testtablespace
	./pg_regress --psqldir=$(PSQLDIR) --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(MAXCONNOPT) $(NOLOCALE)


# old interfaces follow...

runcheck: check
runtest: installcheck
runtest-parallel: installcheck-parallel

bigtest:
	./pg_regress --psqldir=$(PSQLDIR) --schedule=$(srcdir)/serial_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE) numeric_big 

bigcheck:
	./pg_regress --temp-install=./tmp_check --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(MAXCONNOPT) $(NOLOCALE) numeric_big


##
## Clean up
##

clean distclean maintainer-clean: clean-lib
# things built by `all' target
	rm -f $(NAME)$(DLSUFFIX) $(OBJS)
	$(MAKE) -C $(contribdir)/spi clean
	rm -f $(output_files) $(input_files) pg_regress.o pg_regress$(X)
# things created by various check targets
	rm -rf testtablespace
	rm -rf results tmp_check log
	rm -f regression.diffs regression.out regress.out run_check.out
ifeq ($(PORTNAME), cygwin)
	rm -f regress.def
endif
ifdef VPATH
	rm -f $(remaining_files_build)
endif
