subdir = src/interfaces/ecpg/test/expected
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

# port number for temp-installation test postmaster
# this is also defined in ../Makefile
TEMP_PORT = 5$(DEF_PGPORT)

connect-test1.c: connect-test1.c.in
	sed -e 's,@TEMP_PORT@,$(TEMP_PORT),g' \
	  $< >$@

all: connect-test1.c

extraclean:
	rm -f connect-test1.c

