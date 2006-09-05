# $PostgreSQL: pgsql/contrib/Makefile,v 1.69 2006/09/05 18:00:57 teodor Exp $

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
		hstore			\
		intagg		\
		intarray	\
		isbn_issn	\
		lo		\
		ltree		\
		oid2name	\
		pg_buffercache	\
		pg_freespacemap \
		pg_trgm		\
		pgbench		\
		pgcrypto	\
		pgrowlocks	\
		pgstattuple	\
		seg		\
		spi		\
		tablefunc	\
		tsearch2	\
		vacuumlo

ifeq ($(with_openssl),yes)
WANTED_DIRS += sslinfo
endif

# Missing:
#		start-scripts	\ (does not have a makefile)
#		xml2		\ (requires libxml installed)


all install installdirs uninstall distprep clean distclean maintainer-clean:
	@for dir in $(WANTED_DIRS); do \
		$(MAKE) -C $$dir $@ || exit; \
	done

# We'd like check operations to run all the subtests before failing;
# also insert a sleep to ensure the previous test backend exited before
# we try to drop the regression database.
check installcheck:
	@CHECKERR=0; for dir in $(WANTED_DIRS); do \
		sleep 1; \
		$(MAKE) -C $$dir $@ || CHECKERR=$$?; \
	done; \
	exit $$CHECKERR
