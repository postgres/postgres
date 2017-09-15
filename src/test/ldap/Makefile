#-------------------------------------------------------------------------
#
# Makefile for src/test/ldap
#
# Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/ldap/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/ldap
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

check:
	$(prove_check)

clean distclean maintainer-clean:
	rm -rf tmp_check
