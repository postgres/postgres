# $PostgreSQL: pgsql/contrib/Makefile,v 1.54 2005/03/12 15:36:24 neilc Exp $

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
		dbsize		\
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		fuzzystrmatch	\
		intagg		\
		intarray	\
		isbn_issn	\
		lo		\
		ltree		\
		miscutil	\
		mysql		\
		noupdate	\
		oid2name	\
		pg_autovacuum	\
		pg_buffercache	\
		pg_dumplo	\
		pg_trgm		\
		pgbench		\
		pgcrypto	\
		pgstattuple	\
		rtree_gist	\
		seg		\
		spi		\
		string		\
		tablefunc	\
		tips		\
		tsearch		\
		tsearch2	\
		userlock	\
		vacuumlo

# Missing:
#		adddepend	\ (does not have a makefile)
#		array		\ (removed all but the README)
#		ipc_check	\ (does not have a makefile)
#		mSQL-interface	\ (requires msql installed)
#		mac		\ (does not have a makefile)
#		oracle		\ (does not have a makefile)
#		pg_upgrade	\ (does not have a makefile)
#		reindexdb	\ (does not have a makefile)
#		start-scripts	\ (does not have a makefile)
#		tools		\ (does not have a makefile)
#		xml		\ (non-standard makefile)
#		xml2		\ (non-standard makefile)


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
