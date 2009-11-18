# $PostgreSQL: pgsql/contrib/Makefile,v 1.90 2009/11/18 21:57:56 tgl Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		adminpack	\
		auto_explain	\
		btree_gin	\
		btree_gist	\
		chkpass		\
		citext		\
		cube		\
		dblink		\
		dict_int	\
		dict_xsyn	\
		earthdistance	\
		fuzzystrmatch	\
		hstore		\
		intagg		\
		intarray	\
		isn		\
		lo		\
		ltree		\
		oid2name	\
		pageinspect	\
		passwordcheck	\
		pg_buffercache	\
		pg_freespacemap \
		pg_standby	\
		pg_stat_statements \
		pg_trgm		\
		pgbench		\
		pgcrypto	\
		pgrowlocks	\
		pgstattuple	\
		seg		\
		spi		\
		tablefunc	\
		test_parser	\
		tsearch2	\
		unaccent	\
		vacuumlo

ifeq ($(with_openssl),yes)
SUBDIRS += sslinfo
endif

ifeq ($(with_ossp_uuid),yes)
SUBDIRS += uuid-ossp
endif

ifeq ($(with_libxml),yes)
SUBDIRS += xml2
endif

# Missing:
#		start-scripts	\ (does not have a makefile)


all install installdirs uninstall distprep clean distclean maintainer-clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $@ || exit; \
	done

# We'd like check operations to run all the subtests before failing.
check installcheck:
	@CHECKERR=0; for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $@ || CHECKERR=$$?; \
	done; \
	exit $$CHECKERR
