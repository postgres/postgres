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
#   os2client

all:
	for dir in *; do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done

.DEFAULT:
	for dir in *; do \
	    if [ -e $$dir/Makefile ]; then \
		$(MAKE) -C $$dir $@ ; \
	    fi; \
	done
