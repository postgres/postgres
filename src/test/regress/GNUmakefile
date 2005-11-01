#-------------------------------------------------------------------------
#
# GNUmakefile--
#    Makefile for src/test/regress (the regression tests)
#
# Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/test/regress/GNUmakefile,v 1.53 2005/11/01 15:09:11 adunstan Exp $
#
#-------------------------------------------------------------------------

subdir = src/test/regress
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

contribdir := $(top_builddir)/contrib

# port number for temp-installation test postmaster
TEMP_PORT = 5$(DEF_PGPORT)

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

##
## Prepare for tests
##

# Build regression test driver

all: pg_regress

pg_regress: pg_regress.sh GNUmakefile $(top_builddir)/src/Makefile.global
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e 's,@pkglibdir@,$(pkglibdir),g' \
	    -e 's,@datadir@,$(datadir),g' \
	    -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@host_tuple@/$(host_tuple)/g' \
	    -e 's,@GMAKE@,$(MAKE),g' \
	    -e 's/@enable_shared@/$(enable_shared)/g' \
	    -e 's/@GCC@/$(GCC)/g' \
	  $< >$@
	chmod a+x $@


# Build dynamically-loaded object file for CREATE FUNCTION ... LANGUAGE 'C'.

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
	$(SHELL) ./pg_regress --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(MAXCONNOPT) $(NOLOCALE)

installcheck: all
	-rm -rf ./testtablespace
	mkdir ./testtablespace
	$(SHELL) ./pg_regress --schedule=$(srcdir)/serial_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE)

installcheck-parallel: all
	-rm -rf ./testtablespace
	mkdir ./testtablespace
	$(SHELL) ./pg_regress --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(MAXCONNOPT) $(NOLOCALE)


# old interfaces follow...

runcheck: check
runtest: installcheck
runtest-parallel: installcheck-parallel

bigtest:
	$(SHELL) ./pg_regress --schedule=$(srcdir)/serial_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(NOLOCALE) numeric_big 

bigcheck:
	$(SHELL) ./pg_regress --temp-install --top-builddir=$(top_builddir) --temp-port=$(TEMP_PORT) --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE) --load-language=plpgsql $(MAXCONNOPT) $(NOLOCALE) numeric_big


##
## Clean up
##

clean distclean maintainer-clean: clean-lib
# things built by `all' target
	rm -f $(NAME)$(DLSUFFIX) $(OBJS)
	$(MAKE) -C $(contribdir)/spi clean
	rm -f $(output_files) $(input_files) pg_regress
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
