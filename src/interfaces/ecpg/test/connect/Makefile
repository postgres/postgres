subdir = src/interfaces/ecpg/test/connect
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

# port number for temp-installation test postmaster
# this is also defined in ../Makefile
TEMP_PORT = 4$(DEF_PGPORT)

test1.pgc: test1.pgc.in
	sed -e 's,@TEMP_PORT@,$(TEMP_PORT),g' \
	  $< >$@
	sed -e 's,@TEMP_PORT@,$(TEMP_PORT),g' \
	  $(top_srcdir)/$(subdir)/../expected/connect-test1.c.in >../expected/connect-test1.c

TESTS = test1 test1.c \
        test2 test2.c \
        test3 test3.c \
        test4 test4.c \
        test5 test5.c

all: $(TESTS)

extraclean:
	rm -f test1.pgc
