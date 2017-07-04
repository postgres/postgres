#-------------------------------------------------------------------------
#
# Makefile for src/test/subscription
#
# Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/subscription/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/subscription
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

EXTRA_INSTALL = contrib/hstore

check:
	$(prove_check)

clean distclean maintainer-clean:
	rm -rf tmp_check
