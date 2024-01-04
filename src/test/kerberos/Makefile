#-------------------------------------------------------------------------
#
# Makefile for src/test/kerberos
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/kerberos/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/kerberos
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

EXTRA_INSTALL += contrib/postgres_fdw
EXTRA_INSTALL += contrib/dblink

export with_gssapi with_krb_srvnam

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)

clean distclean:
	rm -rf tmp_check
