# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.38 2002/09/05 18:40:33 petere Exp $

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
		userlock	\
		vacuumlo

#		findoidjoins	\
		tsearch		\

ifeq ($(with_java),yes)
WANTED_DIRS += retep
endif

# Missing:
#		ipc_check	\ (does not have a makefile)
#		mSQL-interface	\ (requires msql installed)
#		mac		\ (does not have a makefile)
#		mysql		\ (does not have a makefile)
#		oracle		\ (does not have a makefile)
#		start-scripts	\ (does not have a makefile)
#		tools		\ (does not have a makefile)
#		xml		\ (non-standard makefile)


all install installdirs uninstall clean distclean maintainer-clean check installcheck:
	@for dir in $(WANTED_DIRS); do \
		$(MAKE) -C $$dir $@ || exit; \
	done
