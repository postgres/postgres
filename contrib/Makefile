# Makefile for contrib code
#
# The following subdirs don't have a Makefile:
#
#   apache_logging
#   linux
#   mSQL-interface
#   noupdate
#   unixdate
#
# The following subdirs give make errors:
#
#   earthdistance
#   findoidjoins
#   isbn_issn
#   os2client
#   pginterface

all:	
	for dir in *; do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir; \
	    fi; \
	done

install:	
	for dir in *; do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done

clean:	
	for dir in *; do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done || exit 0

distclean:
	for dir in *; do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done || exit 0
