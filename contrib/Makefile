# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.12 2001/01/15 22:20:00 tgl Exp $

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
		linux		\
		lo			\
		mSQL-interface	\
		mac			\
		miscutil	\
		noupdate	\
		pg_dumplo	\
		pgbench		\
		pgcrypto	\
		rserv		\
		seg			\
		soundex		\
		spi			\
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
