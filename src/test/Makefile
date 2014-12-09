#-------------------------------------------------------------------------
#
# Makefile for src/test
#
# Copyright (c) 1994, Regents of the University of California
#
# src/test/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = regress isolation modules

# The SSL suite is not secure to run on a multi-user system, so don't run
# it as part of global "check" target.
ALWAYS_SUBDIRS = ssl

# We want to recurse to all subdirs for all standard targets, except that
# installcheck and install should not recurse into the subdirectory "modules".

recurse_alldirs_targets := $(filter-out installcheck install, $(standard_targets))
installable_dirs := $(filter-out modules, $(SUBDIRS))

$(call recurse,$(recurse_alldirs_targets))
$(call recurse,installcheck, $(installable_dirs))
$(call recurse,install, $(installable_dirs))

$(recurse_always)
