subdir = src/interfaces/ecpg
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = include pgtypeslib ecpglib compatlib preproc

# Suppress parallel build of subdirectories to avoid a bug in GNU make 3.82, cf
# http://savannah.gnu.org/bugs/?30653
# https://bugzilla.redhat.com/show_bug.cgi?id=835424
# (There are some other parallelism bugs in the subdirectory makefiles
# themselves, but there's little point in fixing them as long as we have
# to use this big hammer.)
ifeq ($(MAKE_VERSION),3.82)
.NOTPARALLEL:
endif

$(recurse)

all-pgtypeslib-recurse all-ecpglib-recurse all-compatlib-recurse all-preproc-recurse: all-include-recurse
all-compatlib-recurse: all-ecpglib-recurse
all-ecpglib-recurse: all-pgtypeslib-recurse
install-pgtypeslib-recurse install-ecpglib-recurse install-compatlib-recurse install-preproc-recurse: install-include-recurse
install-compatlib-recurse: install-ecpglib-recurse
install-ecpglib-recurse: install-pgtypeslib-recurse

clean distclean maintainer-clean:
	$(MAKE) -C test clean

checktcp: | temp-install
check checktcp installcheck:
	$(MAKE) -C test $@
