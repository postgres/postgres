subdir = src/interfaces/ecpg
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = include pgtypeslib ecpglib compatlib preproc

$(recurse)

all-compatlib-recursive: all-ecpglib-recursive

clean distclean maintainer-clean:
	$(MAKE) -C test clean

check checktcp installcheck: all
	$(MAKE) -C test $@
