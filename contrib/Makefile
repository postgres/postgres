#
# The PostgreSQL contrib tree Makefile
#
# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.7 2000/07/16 14:50:29 petere Exp $
#

subdir = contrib
top_builddir = ..
include ../src/Makefile.global

WANTED_DIRS =	array		\
		earthdistance	\
		findoidjoins	\
		fulltextindex	\
		isbn_issn	\
		linux		\
		lo		\
		mSQL-interface	\
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
	#	odbc

all install installdirs uninstall clean distclean maintainer-clean:
	for dir in $(WANTED_DIRS); do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done

install:
	for dir in $(WANTED_DIRS); do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done
	$(INSTALL_DATA) Contrib.index $(docdir)/contrib
