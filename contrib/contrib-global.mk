# $PostgreSQL: pgsql/contrib/contrib-global.mk,v 1.9 2005/05/17 18:26:22 tgl Exp $

NO_PGXS = 1
REGRESS_OPTS = --dbname=$(CONTRIB_TESTDB)
include $(top_srcdir)/src/makefiles/pgxs.mk
