# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.44 2003/06/30 20:02:53 tgl Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
		array		\
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
		noupdate	\
		oid2name	\
		pg_autovacuum	\
		pg_dumplo	\
		pg_logger	\
		pgbench		\
		pgcrypto	\
		pgstattuple	\
		rserv		\
		rtree_gist	\
		seg		\
		spi		\
		string		\
		tablefunc	\
		tips		\
		tsearch		\
		userlock	\
		vacuumlo

# Missing:
#		adddepend	\ (does not have a makefile)
#		ipc_check	\ (does not have a makefile)
#		mSQL-interface	\ (requires msql installed)
#		mac		\ (does not have a makefile)
#		mysql		\ (does not have a makefile)
#		oracle		\ (does not have a makefile)
#		pg_upgrade	\ (does not have a makefile)
#		reindexdb	\ (does not have a makefile)
#		start-scripts	\ (does not have a makefile)
#		tools		\ (does not have a makefile)
#		xml		\ (non-standard makefile)


all install installdirs uninstall clean distclean maintainer-clean check installcheck:
	@for dir in $(WANTED_DIRS); do \
		$(MAKE) -C $$dir $@ || exit; \
	done
