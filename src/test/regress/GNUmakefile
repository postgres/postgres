#-------------------------------------------------------------------------
#
# GNUmakefile--
#    Makefile for regress (the regression tests)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/test/regress/GNUmakefile,v 1.31 2000/11/21 23:40:28 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/test/regress
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

contribdir := $(top_builddir)/contrib

override CPPFLAGS += -I$(libpq_srcdir)
override CFLAGS += $(CFLAGS_SL)


##
## Prepare for tests
##

# Build regression test driver

all: pg_regress

pg_regress: pg_regress.sh GNUmakefile
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@libdir@,$(libdir),g' \
	    -e 's,@datadir@,$(datadir),g' \
	    -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@host_tuple@/$(host_tuple)/g' \
	    -e 's,@GMAKE@,$(MAKE),g' \
	    -e 's/@enable_shared@/$(enable_shared)/g' \
	    -e 's/@GCC@/$(GCC)/g' \
	  $< >$@
	chmod a+x $@


# Build dynamically-loaded object file for CREATE FUNCTION ... LANGUAGE 'C'.

DLOBJS := regress$(DLSUFFIX)
# This is for some platforms
ifdef EXPSUFF
DLOBJS += regress$(EXPSUFF)
endif

all: $(DLOBJS)


# Build test input and expected files

file_list := copy create_function_1 create_function_2 misc constraints
input_files  := $(foreach file, $(file_list), sql/$(file).sql)
output_files := $(foreach file, $(file_list), expected/$(file).out)

all: $(input_files) $(output_files)

abs_srcdir := $(shell cd $(srcdir) && pwd)
abs_builddir := $(shell pwd)

define sed-command
sed -e 's,@abs_srcdir@,$(abs_srcdir),g' \
    -e 's,@abs_builddir@,$(abs_builddir),g' \
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
remaining_files_src := $(wildcard $(srcdir)/sql/*.sql) $(wildcard $(srcdir)/expected/*.out)
remaining_files_build := $(patsubst $(srcdir)/%, $(abs_builddir)/%, $(remaining_files_src))

all: $(remaining_files_build)
$(remaining_files_build): $(abs_builddir)/%: $(srcdir)/%
	ln -s $< $@
endif


# And finally some extra C modules...

all: all-spi

.PHONY: all-spi
all-spi:
	$(MAKE) -C $(contribdir)/spi REFINT_VERBOSE=1 refint$(DLSUFFIX) autoinc$(DLSUFFIX)


##
## Run tests
##

check: all
	$(SHELL) ./pg_regress --temp-install --top-builddir=$(top_builddir) --schedule=$(srcdir)/parallel_schedule --multibyte=$(MULTIBYTE)

installcheck: all
	$(SHELL) ./pg_regress --schedule=$(srcdir)/serial_schedule --multibyte=$(MULTIBYTE)


# old interfaces follow...

#
# huge extra tests run in target bigtest
#
EXTRA_TESTS	= numeric_big

#
# run the tests
#
runtest: all
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./regress.sh $(host_tuple) 2>&1 | tee regress.out
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./regress.sh $(host_tuple) 2>&1 | tee regress.out
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"
	@echo ""
	@echo "To run the optional big test(s) too, type 'make bigtest'"
	@echo "These big tests can take over an hour to complete"
	@echo "These actually are: $(EXTRA_TESTS)"

#
# run the test including the huge extra tests
#
bigtest: all
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	$(SHELL) ./regress.sh $(host_tuple) $(EXTRA_TESTS) 2>&1 | tee regress.out
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	./regress.sh $(host_tuple) $(EXTRA_TESTS) 2>&1 | tee regress.out
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILE regress.out"

#
# run the parallel test suite
#
runcheck: all
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	MAKE=$(MAKE);export MAKE; \
	$(SHELL) ./run_check.sh $(host_tuple)
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	MAKE=$(MAKE);export MAKE; \
	./run_check.sh $(host_tuple)
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILES run_check.out"
	@echo "AND regress.out"
	@echo ""
	@echo "To run the optional big test(s) too, type 'make bigcheck'"
	@echo "These big tests can take over an hour to complete"
	@echo "These actually are: $(EXTRA_TESTS)"

#
# run the test including the huge extra tests
#
bigcheck: all
ifneq ($(PORTNAME), win)
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	MAKE=$(MAKE);export MAKE; \
	$(SHELL) ./run_check.sh $(host_tuple) $(EXTRA_TESTS)
else
	MULTIBYTE=$(MULTIBYTE);export MULTIBYTE; \
	MAKE=$(MAKE);export MAKE; \
	./run_check.sh $(host_tuple) $(EXTRA_TESTS)
endif
	@echo "ACTUAL RESULTS OF REGRESSION TEST ARE NOW IN FILES run_check.out"
	@echo "AND regress.out"


# clean up

clean distclean maintainer-clean:
# things built by `all' target
	$(MAKE) -C $(contribdir)/spi clean
	rm -f $(output_files) $(input_files) $(DLOBJS) regress.o pg_regress
# things created by various check targets
	rm -rf results tmp_check log
	rm -f regression.diffs regression.out regress.out run_check.out
ifeq ($(PORTNAME), win)
	rm -f regress.def
endif
ifdef VPATH
	rm -f $(remaining_files_build)
endif
