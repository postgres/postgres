# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.17 2001/03/13 19:28:02 petere Exp $

subdir = contrib
top_builddir = ..
include $(top_builddir)/src/Makefile.global

WANTED_DIRS = \
		array		\
		cube		\
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		intarray	\
		isbn_issn	\
		lo		\
		mSQL-interface	\
		mac		\
		miscutil	\
		noupdate	\
		oid2name	\
		pg_dumplo	\
		pg_logger	\
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
