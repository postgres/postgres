# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.20 2001/05/10 15:51:05 momjian Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
		array		\
		cube		\
		dbase		\
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		intarray	\
		isbn_issn	\
		lo		\
		mSQL-interface	\
		mac		\
		metaphone	\
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
		seg		\
		soundex		\
		spi		\
		string		\
		tips		\
		unixdate	\
		userlock	\
		vacuumlo	

all install installdirs uninstall clean distclean maintainer-clean:
	for dir in $(WANTED_DIRS); do \
	    if [ -f $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@; \
	    fi; \
	done
