#-------------------------------------------------------------------------
#
# Makefile for src/test/libpq_encryption
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/libpq_encryption/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/libpq_encryption
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export with_ssl with_gssapi with_krb_srvnam

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check
