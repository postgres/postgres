#-------------------------------------------------------------------------
#
# Makefile for src/test/subscription
#
# Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/subscription/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/subscription
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

EXTRA_INSTALL = contrib/hstore

export with_icu

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean maintainer-clean:
	rm -rf tmp_check
