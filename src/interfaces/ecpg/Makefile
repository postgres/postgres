subdir = src/interfaces/ecpg
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = include pgtypeslib ecpglib compatlib preproc

$(recurse)

all-compatlib-recurse: all-ecpglib-recurse
all-ecpglib-recurse: all-pgtypeslib-recurse
install-compatlib-recurse: install-ecpglib-recurse
install-ecpglib-recurse: install-pgtypeslib-recurse

clean distclean maintainer-clean:
	$(MAKE) -C test clean

check checktcp installcheck: all
	$(MAKE) -C test $@
