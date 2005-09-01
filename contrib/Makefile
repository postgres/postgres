# $PostgreSQL: pgsql/contrib/Makefile,v 1.61 2005/09/01 22:02:44 tgl Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
		btree_gist	\
		chkpass		\
		cube		\
		dbase		\
		dblink		\
		dbmirror	\
		earthdistance	\
		fulltextindex	\
		fuzzystrmatch	\
		intagg		\
		intarray	\
		isbn_issn	\
		lo		\
		ltree		\
		oid2name	\
		pg_buffercache	\
		pg_trgm		\
		pgbench		\
		pgcrypto	\
		pgstattuple	\
		seg		\
		spi		\
		tablefunc	\
		tips		\
		tsearch2	\
		userlock	\
		vacuumlo

# Missing:
#		adddepend	\ (does not have a makefile)
#		mSQL-interface	\ (requires msql installed)
#		mac		\ (does not have a makefile)
#		oracle		\ (does not have a makefile)
#		start-scripts	\ (does not have a makefile)
#		xml2		\ (requires libxml installed)


all install installdirs uninstall clean distclean maintainer-clean:
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
