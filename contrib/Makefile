# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.28 2001/09/29 03:11:58 momjian Exp $

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
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		fuzzystrmatch	\
		intarray	\
		isbn_issn	\
		lo		\
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
		userlock	\
		vacuumlo

# Missing:
#		ipc_check	\ (does not have a makefile)
#		mSQL-interface	\ (requires msql installed)
#		mac		\ (does not have a makefile)
#		mysql		\ (does not have a makefile)
#		oracle		\ (does not have a makefile)
#		start-scripts	\ (does not have a makefile)
#		tools		\ (does not have a makefile)
#		xml		\ (non-standard makefile)

ifeq ($(with_java),yes)
WANTED_DIRS += retep
endif


all install installdirs uninstall clean distclean maintainer-clean check installcheck:
	@for dir in $(WANTED_DIRS); do \
		$(MAKE) -C $$dir $@ || exit; \
	done
