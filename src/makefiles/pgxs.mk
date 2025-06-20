# PGXS: PostgreSQL extensions makefile

# src/makefiles/pgxs.mk

# This file contains generic rules to build many kinds of simple
# extension modules.  You only need to set a few variables and include
# this file, the rest will be done here.
#
# Use the following layout for your Makefile:
#
#   [variable assignments, see below]
#
#   PG_CONFIG = pg_config
#   PGXS := $(shell $(PG_CONFIG) --pgxs)
#   include $(PGXS)
#
#   [custom rules, rarely necessary]
#
# Set one of these three variables to specify what is built:
#
#   MODULES -- list of shared-library objects to be built from source files
#     with same stem (do not include library suffixes in this list)
#   MODULE_big -- a shared library to build from multiple source files
#     (list object files in OBJS)
#   PROGRAM -- an executable program to build (list object files in OBJS)
#
# The following variables can also be set:
#
#   EXTENSION -- name of extension (there must be a $EXTENSION.control file)
#   MODULEDIR -- subdirectory of $PREFIX/share into which DATA and DOCS files
#     should be installed (if not set, default is "extension" if EXTENSION
#     is set, or "contrib" if not)
#   DATA -- random files to install into $PREFIX/share/$MODULEDIR
#   DATA_built -- random files to install into $PREFIX/share/$MODULEDIR,
#     which need to be built first
#   DATA_TSEARCH -- random files to install into $PREFIX/share/tsearch_data
#   DOCS -- random files to install under $PREFIX/doc/$MODULEDIR
#   SCRIPTS -- script files (not binaries) to install into $PREFIX/bin
#   SCRIPTS_built -- script files (not binaries) to install into $PREFIX/bin,
#     which need to be built first
#   HEADERS -- files to install into $(includedir_server)/$MODULEDIR/$MODULE_big
#   HEADERS_built -- as above but built first (but NOT cleaned)
#   HEADERS_$(MODULE) -- files to install into
#     $(includedir_server)/$MODULEDIR/$MODULE; the value of $MODULE must be
#     listed in MODULES or MODULE_big
#   HEADERS_built_$(MODULE) -- as above but built first (also NOT cleaned)
#   REGRESS -- list of regression test cases (without suffix)
#   REGRESS_OPTS -- additional switches to pass to pg_regress
#   TAP_TESTS -- switch to enable TAP tests
#   ISOLATION -- list of isolation test cases
#   ISOLATION_OPTS -- additional switches to pass to pg_isolation_regress
#   NO_INSTALL -- don't define an install target, useful for test modules
#     that don't need their build products to be installed
#   NO_INSTALLCHECK -- don't define an installcheck target, useful e.g. if
#     tests require special configuration, or don't use pg_regress
#   EXTRA_CLEAN -- extra files to remove in 'make clean'
#   PG_CPPFLAGS -- will be prepended to CPPFLAGS
#   PG_CFLAGS -- will be appended to CFLAGS
#   PG_CXXFLAGS -- will be appended to CXXFLAGS
#   PG_LDFLAGS -- will be prepended to LDFLAGS
#   PG_LIBS -- will be added to PROGRAM link line
#   PG_LIBS_INTERNAL -- same, for references to libraries within build tree
#   SHLIB_LINK -- will be added to MODULE_big link line
#   SHLIB_LINK_INTERNAL -- same, for references to libraries within build tree
#   PG_CONFIG -- path to pg_config program for the PostgreSQL installation
#     to build against (typically just "pg_config" to use the first one in
#     your PATH)
#
# Better look at some of the existing uses for examples...

ifndef PGXS
ifndef NO_PGXS
$(error pgxs error: makefile variable PGXS or NO_PGXS must be set)
endif
endif


ifdef PGXS

# External extensions must assume generated headers are available
NO_GENERATED_HEADERS=yes
# The temp-install rule won't work, either
NO_TEMP_INSTALL=yes

# We assume that we are in src/makefiles/, so top is ...
top_builddir := $(dir $(PGXS))../..
include $(top_builddir)/src/Makefile.global

# These might be set in Makefile.global, but if they were not found
# during the build of PostgreSQL, supply default values so that users
# of pgxs can use the variables.
ifeq ($(BISON),)
BISON = bison
endif
ifeq ($(FLEX),)
FLEX = flex
endif

endif # PGXS


override CPPFLAGS := -I. -I$(srcdir) $(CPPFLAGS)

# See equivalent block in Makefile.shlib
ifdef MODULES
override LDFLAGS_SL += $(CFLAGS_SL_MODULE)
override CFLAGS += $(CFLAGS_SL) $(CFLAGS_SL_MODULE)
override CXXFLAGS += $(CFLAGS_SL) $(CXXFLAGS_SL_MODULE)
endif

ifdef MODULEDIR
datamoduledir := $(MODULEDIR)
docmoduledir := $(MODULEDIR)
incmoduledir := $(MODULEDIR)
else
ifdef EXTENSION
datamoduledir := extension
docmoduledir := extension
incmoduledir := extension
else
datamoduledir := contrib
docmoduledir := contrib
incmoduledir := contrib
endif
endif

ifdef PG_CPPFLAGS
override CPPFLAGS := $(PG_CPPFLAGS) $(CPPFLAGS)
endif
ifdef PG_CFLAGS
override CFLAGS := $(CFLAGS) $(PG_CFLAGS)
endif
ifdef PG_CXXFLAGS
override CXXFLAGS := $(CXXFLAGS) $(PG_CXXFLAGS)
endif
ifdef PG_LDFLAGS
override LDFLAGS := $(PG_LDFLAGS) $(LDFLAGS)
endif

# logic for HEADERS_* stuff

# get list of all names used with or without built_ prefix
# note that use of HEADERS_built_foo will get both "foo" and "built_foo",
# we cope with that later when filtering this list against MODULES.
# If someone wants to name a module "built_foo", they can do that and it
# works, but if they have MODULES = foo built_foo  then they will need to
# force building of all headers and use HEADERS_built_foo and
# HEADERS_built_built_foo.
HEADER_alldirs := $(patsubst HEADERS_%,%,$(filter HEADERS_%, $(.VARIABLES)))
HEADER_alldirs += $(patsubst HEADERS_built_%,%,$(filter HEADERS_built_%, $(.VARIABLES)))

# collect all names of built headers to use as a dependency
HEADER_allbuilt :=

ifdef MODULE_big

# we can unconditionally add $(MODULE_big) here, because we will strip it
# back out below if it turns out not to actually define any headers.
HEADER_dirs := $(MODULE_big)
HEADER_unbuilt_$(MODULE_big) = $(HEADERS)
HEADER_built_$(MODULE_big) = $(HEADERS_built)
HEADER_allbuilt += $(HEADERS_built)
# treat "built" as an exclusion below as well as "built_foo"
HEADER_xdirs := built built_$(MODULE_big)

else # not MODULE_big, so check MODULES

# HEADERS is an error in the absence of MODULE_big to provide a dir name
ifdef HEADERS
$(error HEADERS requires MODULE_big to be set)
endif
# make list of modules that have either HEADERS_foo or HEADERS_built_foo
HEADER_dirs := $(foreach m,$(MODULES),$(if $(filter $(m) built_$(m),$(HEADER_alldirs)),$(m)))
# make list of conflicting names to exclude
HEADER_xdirs := $(addprefix built_,$(HEADER_dirs))

endif # MODULE_big or MODULES

# HEADERS_foo requires that "foo" is in MODULES as a sanity check
ifneq (,$(filter-out $(HEADER_dirs) $(HEADER_xdirs),$(HEADER_alldirs)))
$(error $(patsubst %,HEADERS_%,$(filter-out $(HEADER_dirs) $(HEADER_xdirs),$(HEADER_alldirs))) defined with no module)
endif

# assign HEADER_unbuilt_foo and HEADER_built_foo, but make sure
# that "built" takes precedence in the case of conflict, by removing
# conflicting module names when matching the unbuilt name
$(foreach m,$(filter-out $(HEADER_xdirs),$(HEADER_dirs)),$(eval HEADER_unbuilt_$(m) += $$(HEADERS_$(m))))
$(foreach m,$(HEADER_dirs),$(eval HEADER_built_$(m) += $$(HEADERS_built_$(m))))
$(foreach m,$(HEADER_dirs),$(eval HEADER_allbuilt += $$(HEADERS_built_$(m))))

# expand out the list of headers for each dir, attaching source prefixes
header_file_list = $(HEADER_built_$(1)) $(addprefix $(srcdir)/,$(HEADER_unbuilt_$(1)))
$(foreach m,$(HEADER_dirs),$(eval HEADER_files_$(m) := $$(call header_file_list,$$(m))))

# note that the caller's HEADERS* vars have all been expanded now, and
# later changes will have no effect.

# remove entries in HEADER_dirs that produced an empty list of files,
# to ensure we don't try and install them
HEADER_dirs := $(foreach m,$(HEADER_dirs),$(if $(strip $(HEADER_files_$(m))),$(m)))

# Functions for generating install/uninstall commands; the blank lines
# before the "endef" are required, don't lose them
# $(call install_headers,dir,headers)
define install_headers
$(MKDIR_P) '$(DESTDIR)$(includedir_server)/$(incmoduledir)/$(1)/'
$(INSTALL_DATA) $(2) '$(DESTDIR)$(includedir_server)/$(incmoduledir)/$(1)/'

endef
# $(call uninstall_headers,dir,headers)
define uninstall_headers
rm -f $(addprefix '$(DESTDIR)$(includedir_server)/$(incmoduledir)/$(1)'/, $(notdir $(2)))

endef

# end of HEADERS_* stuff


all: $(PROGRAM) $(DATA_built) $(HEADER_allbuilt) $(SCRIPTS_built) $(addsuffix $(DLSUFFIX), $(MODULES)) $(addsuffix .control, $(EXTENSION))

ifeq ($(with_llvm), yes)
all: $(addsuffix .bc, $(MODULES)) $(patsubst %.o,%.bc, $(OBJS))
endif

ifdef MODULE_big
# shared library parameters
NAME = $(MODULE_big)

include $(top_srcdir)/src/Makefile.shlib

all: all-lib
endif # MODULE_big


ifndef NO_INSTALL

install: all installdirs
ifneq (,$(EXTENSION))
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(addsuffix .control, $(EXTENSION))) '$(DESTDIR)$(datadir)/extension/'
endif # EXTENSION
ifneq (,$(DATA)$(DATA_built))
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA)) $(DATA_built) '$(DESTDIR)$(datadir)/$(datamoduledir)/'
endif # DATA
ifneq (,$(DATA_TSEARCH))
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DATA_TSEARCH)) '$(DESTDIR)$(datadir)/tsearch_data/'
endif # DATA_TSEARCH
ifdef MODULES
	$(INSTALL_SHLIB) $(addsuffix $(DLSUFFIX), $(MODULES)) '$(DESTDIR)$(pkglibdir)/'
ifeq ($(with_llvm), yes)
	$(foreach mod, $(MODULES), $(call install_llvm_module,$(mod),$(mod).bc))
endif # with_llvm
endif # MODULES
ifdef DOCS
ifdef docdir
	$(INSTALL_DATA) $(addprefix $(srcdir)/, $(DOCS)) '$(DESTDIR)$(docdir)/$(docmoduledir)/'
endif # docdir
endif # DOCS
ifdef PROGRAM
	$(INSTALL_PROGRAM) $(PROGRAM)$(X) '$(DESTDIR)$(bindir)'
endif # PROGRAM
ifdef SCRIPTS
	$(INSTALL_SCRIPT) $(addprefix $(srcdir)/, $(SCRIPTS)) '$(DESTDIR)$(bindir)/'
endif # SCRIPTS
ifdef SCRIPTS_built
	$(INSTALL_SCRIPT) $(SCRIPTS_built) '$(DESTDIR)$(bindir)/'
endif # SCRIPTS_built
ifneq (,$(strip $(HEADER_dirs)))
	$(foreach dir,$(HEADER_dirs),$(call install_headers,$(dir),$(HEADER_files_$(dir))))
endif # HEADERS
ifdef MODULE_big
ifeq ($(with_llvm), yes)
	$(call install_llvm_module,$(MODULE_big),$(OBJS))
endif # with_llvm

install: install-lib
endif # MODULE_big


installdirs:
ifneq (,$(EXTENSION))
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'
endif
ifneq (,$(DATA)$(DATA_built))
	$(MKDIR_P) '$(DESTDIR)$(datadir)/$(datamoduledir)'
endif
ifneq (,$(DATA_TSEARCH))
	$(MKDIR_P) '$(DESTDIR)$(datadir)/tsearch_data'
endif
ifneq (,$(MODULES))
	$(MKDIR_P) '$(DESTDIR)$(pkglibdir)'
endif
ifdef DOCS
ifdef docdir
	$(MKDIR_P) '$(DESTDIR)$(docdir)/$(docmoduledir)'
endif # docdir
endif # DOCS
ifneq (,$(PROGRAM)$(SCRIPTS)$(SCRIPTS_built))
	$(MKDIR_P) '$(DESTDIR)$(bindir)'
endif

ifdef MODULE_big
installdirs: installdirs-lib
endif # MODULE_big


uninstall:
ifneq (,$(EXTENSION))
	rm -f $(addprefix '$(DESTDIR)$(datadir)/extension'/, $(notdir $(addsuffix .control, $(EXTENSION))))
endif
ifneq (,$(DATA)$(DATA_built))
	rm -f $(addprefix '$(DESTDIR)$(datadir)/$(datamoduledir)'/, $(notdir $(DATA) $(DATA_built)))
endif
ifneq (,$(DATA_TSEARCH))
	rm -f $(addprefix '$(DESTDIR)$(datadir)/tsearch_data'/, $(notdir $(DATA_TSEARCH)))
endif
ifdef MODULES
	rm -f $(addprefix '$(DESTDIR)$(pkglibdir)'/, $(addsuffix $(DLSUFFIX), $(MODULES)))
ifeq ($(with_llvm), yes)
	$(foreach mod, $(MODULES), $(call uninstall_llvm_module,$(mod)))
endif # with_llvm
endif # MODULES
ifdef DOCS
	rm -f $(addprefix '$(DESTDIR)$(docdir)/$(docmoduledir)'/, $(DOCS))
endif
ifdef PROGRAM
	rm -f '$(DESTDIR)$(bindir)/$(PROGRAM)$(X)'
endif
ifdef SCRIPTS
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, $(SCRIPTS))
endif
ifdef SCRIPTS_built
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, $(SCRIPTS_built))
endif
ifneq (,$(strip $(HEADER_dirs)))
	$(foreach dir,$(HEADER_dirs),$(call uninstall_headers,$(dir),$(HEADER_files_$(dir))))
endif # HEADERS

ifdef MODULE_big
ifeq ($(with_llvm), yes)
	$(call uninstall_llvm_module,$(MODULE_big))
endif # with_llvm

uninstall: uninstall-lib
endif # MODULE_big

else # NO_INSTALL

# Need this so that temp-install builds artifacts not meant for
# installation (Normally, check should depend on all, but we don't do
# that because of parallel make risk (dbf2ec1a1c0).)
install: all

endif # NO_INSTALL


clean:
ifdef MODULES
	rm -f $(addsuffix $(DLSUFFIX), $(MODULES)) $(addsuffix .o, $(MODULES)) $(if $(PGFILEDESC),$(WIN32RES)) \
	    $(addsuffix .bc, $(MODULES))
endif
ifdef DATA_built
	rm -f $(DATA_built)
endif
ifdef SCRIPTS_built
	rm -f $(SCRIPTS_built)
endif
ifdef PROGRAM
	rm -f $(PROGRAM)$(X)
endif
ifdef OBJS
	rm -f $(OBJS) $(patsubst %.o,%.bc, $(OBJS))
endif
ifdef EXTRA_CLEAN
	rm -rf $(EXTRA_CLEAN)
endif
ifdef REGRESS
# things created by various check targets
	rm -rf $(pg_regress_clean_files)
endif
ifdef TAP_TESTS
	rm -rf tmp_check/
endif
ifdef ISOLATION
	rm -rf output_iso/ tmp_check_iso/
endif

ifdef MODULE_big
clean: clean-lib
endif

distclean: clean


ifdef REGRESS

REGRESS_OPTS += --dbname=$(CONTRIB_TESTDB)

# When doing a VPATH build, must copy over the data files so that the
# driver script can find them.  We have to use an absolute path for
# the targets, because otherwise make will try to locate the missing
# files using VPATH, and will find them in $(srcdir), but the point
# here is that we want to copy them from $(srcdir) to the build
# directory.

ifdef VPATH
abs_builddir := $(shell pwd)
test_files_src := $(wildcard $(srcdir)/data/*.data)
test_files_build := $(patsubst $(srcdir)/%, $(abs_builddir)/%, $(test_files_src))

all: $(test_files_build)
$(test_files_build): $(abs_builddir)/%: $(srcdir)/%
	$(MKDIR_P) $(dir $@)
	ln -s $< $@
endif # VPATH
endif # REGRESS

.PHONY: submake
submake:
ifndef PGXS
	$(MAKE) -C $(top_builddir)/src/test/regress pg_regress$(X)
	$(MAKE) -C $(top_builddir)/src/test/isolation all
endif

ifdef ISOLATION
ISOLATION_OPTS += --dbname=$(ISOLATION_TESTDB)
endif

# Standard rules to run regression tests including multiple test suites.
# Runs against an installed postmaster.
ifndef NO_INSTALLCHECK
installcheck: submake $(REGRESS_PREP)
ifdef REGRESS
	$(pg_regress_installcheck) $(REGRESS_OPTS) $(REGRESS)
endif
ifdef ISOLATION
	$(pg_isolation_regress_installcheck) $(ISOLATION_OPTS) $(ISOLATION)
endif
ifdef TAP_TESTS
	$(prove_installcheck)
endif
endif # NO_INSTALLCHECK

# Runs independently of any installation
ifdef PGXS
check:
	@echo '"$(MAKE) check" is not supported.'
	@echo 'Do "$(MAKE) install", then "$(MAKE) installcheck" instead.'
else
check: submake $(REGRESS_PREP)
ifdef REGRESS
	$(pg_regress_check) $(REGRESS_OPTS) $(REGRESS)
endif
ifdef ISOLATION
	$(pg_isolation_regress_check) $(ISOLATION_OPTS) $(ISOLATION)
endif
ifdef TAP_TESTS
	$(prove_check)
endif
endif # PGXS

ifndef NO_TEMP_INSTALL
checkprep: EXTRA_INSTALL+=$(subdir)
endif


# STANDARD RULES

ifneq (,$(MODULES)$(MODULE_big))
%.sql: %.sql.in
	sed 's,MODULE_PATHNAME,$$libdir/$*,g' $< >$@
endif

ifdef PROGRAM
$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(PG_LIBS_INTERNAL) $(LDFLAGS) $(LDFLAGS_EX) $(PG_LIBS) $(LIBS) -o $@$(X)
endif
