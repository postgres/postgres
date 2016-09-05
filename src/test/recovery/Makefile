#-------------------------------------------------------------------------
#
# Makefile for src/test/recovery
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/recovery/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/recovery
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

clean distclean maintainer-clean:
	rm -rf tmp_check

EXTRA_INSTALL = contrib/test_decoding
