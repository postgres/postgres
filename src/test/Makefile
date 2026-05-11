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

SUBDIRS = perl postmaster regress isolation modules authentication recovery subscription

ifeq ($(with_icu),yes)
SUBDIRS += icu
endif
ifeq ($(with_gssapi),yes)
SUBDIRS += kerberos
endif
ifeq ($(with_ldap),yes)
SUBDIRS += ldap
endif
ifeq ($(with_ssl),openssl)
SUBDIRS += ssl
endif

# Test suites that are not safe by default but can be run if selected
# by the user via the whitespace-separated list in variable PG_TEST_EXTRA.
# Export PG_TEST_EXTRA to check it in individual tap tests.
export PG_TEST_EXTRA

# We don't build or execute these by default, but we do want "make
# clean" etc to recurse into them.  (We must filter out those that we
# have conditionally included into SUBDIRS above, else there will be
# make confusion.)
ALWAYS_SUBDIRS = $(filter-out $(SUBDIRS),examples kerberos icu ldap ssl)

# We want to recurse to all subdirs for all standard targets, except that
# installcheck and install should not recurse into the subdirectory "modules".

recurse_alldirs_targets := $(filter-out installcheck install, $(standard_targets))
installable_dirs := $(filter-out modules, $(SUBDIRS))

$(call recurse,$(recurse_alldirs_targets))
$(call recurse,installcheck, $(installable_dirs))
$(call recurse,install, $(installable_dirs))

$(recurse_always)
