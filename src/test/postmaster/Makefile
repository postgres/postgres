#-------------------------------------------------------------------------
#
# Makefile for src/test/postmaster
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/postmaster/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/postmaster
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check
