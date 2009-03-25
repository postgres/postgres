# $PostgreSQL: pgsql/contrib/Makefile,v 1.87 2009/03/25 23:20:01 tgl Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
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
		vacuumlo

ifeq ($(with_openssl),yes)
WANTED_DIRS += sslinfo
endif

ifeq ($(with_ossp_uuid),yes)
WANTED_DIRS += uuid-ossp
endif

ifeq ($(with_libxml),yes)
WANTED_DIRS += xml2
endif

# Missing:
#		start-scripts	\ (does not have a makefile)


all install installdirs uninstall distprep clean distclean maintainer-clean:
	@for dir in $(WANTED_DIRS); do \
		$(MAKE) -C $$dir $@ || exit; \
	done

# We'd like check operations to run all the subtests before failing.
check installcheck:
	@CHECKERR=0; for dir in $(WANTED_DIRS); do \
		$(MAKE) -C $$dir $@ || CHECKERR=$$?; \
	done; \
	exit $$CHECKERR
