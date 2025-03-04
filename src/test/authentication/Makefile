#-------------------------------------------------------------------------
#
# Makefile for src/test/authentication
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/authentication/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/authentication
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

EXTRA_INSTALL = src/test/modules/injection_points

export enable_injection_points

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check
