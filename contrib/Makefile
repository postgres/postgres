# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.8 2000/09/18 20:11:34 petere Exp $

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
