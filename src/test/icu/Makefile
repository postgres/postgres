#-------------------------------------------------------------------------
#
# Makefile for src/test/icu
#
# Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/icu/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/icu
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export with_icu

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean maintainer-clean:
	rm -rf tmp_check
