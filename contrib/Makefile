#
# The PostgreSQL contrib tree Makefile
#
#	Portions Copyright (c) 1999-2000, PostgreSQL, Inc
#
# $Header: /cvsroot/pgsql/contrib/Makefile,v 1.5 2000/06/19 13:52:59 momjian Exp $
#

TOPDIR = ..
include Makefile.global

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
		tools		\
		unixdate	\
		userlock	\
		vacuumlo	
	#	odbc


all:
	for dir in $(WANTED_DIRS); do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done


install:
	../config/mkinstalldirs $(CONTRIB_BINDIR) $(CONTRIB_MODDIR) $(CONTRIB_DOCDIR) $(CONTRIB_SQLDIR) $(CONTRIB_EXAMPLESDIR)
	for dir in $(WANTED_DIRS); do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done
	$(INSTALL) $(INSTL_LIB_OPTS) Contrib.index $(CONTRIB_DOCDIR);


.DEFAULT:
	for dir in $(WANTED_DIRS); do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done
