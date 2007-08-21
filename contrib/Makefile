# $PostgreSQL: pgsql/contrib/Makefile,v 1.78 2007/08/21 01:12:28 tgl Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
		adminpack	\
		btree_gist	\
		chkpass		\
		cube		\
		dblink		\
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
		pg_trgm		\
		pgbench		\
		pgcrypto	\
		pgrowlocks	\
		pgstattuple	\
		seg		\
		spi		\
		tablefunc	\
		vacuumlo

ifeq ($(with_openssl),yes)
WANTED_DIRS += sslinfo
endif

ifeq ($(with_ossp_uuid),yes)
WANTED_DIRS += uuid-ossp
endif

ifeq ($(with_libxml),yes)
ifeq ($(with_libxslt),yes)
WANTED_DIRS += xml2
endif
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
