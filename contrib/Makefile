# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.24 2001/08/22 18:27:53 tgl Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
		array		\
		btree_gist	\
		cube		\
		dbase		\
		dblink		\
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		fuzzystrmatch	\
		intarray	\
		isbn_issn	\
		lo		\
		mSQL-interface	\
		mac		\
		miscutil	\
		noupdate	\
		oid2name	\
		pg_controldata	\
		pg_dumplo	\
		pg_logger	\
		pg_resetxlog	\
		pgbench		\
		pgcrypto	\
		rserv		\
		rtree_gist	\
		seg		\
		spi		\
		string		\
		tips		\
		unixdate	\
		userlock	\
		vacuumlo	

ifeq ($(with_java),yes)
WANTED_DIRS += retep
endif


all install installdirs uninstall clean distclean maintainer-clean:
	for dir in $(WANTED_DIRS); do \
	    if [ -f $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@; \
	    fi; \
	done
