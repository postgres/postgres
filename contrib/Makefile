# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.9 2000/10/31 13:11:28 petere Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS =	array		\
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		isbn_issn	\
		linux		\
		lo		\
		mSQL-interface	\
		mac		\
		miscutil	\
		noupdate	\
		pg_dumplo	\
		pgbench		\
		pgcrypto	\
		soundex		\
		spi		\
		string		\
		tips		\
		unixdate	\
		userlock	\
		vacuumlo	

all install installdirs uninstall clean distclean maintainer-clean:
	for dir in $(WANTED_DIRS); do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@; \
	    fi; \
	done
