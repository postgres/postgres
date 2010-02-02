subdir = src/interfaces/ecpg/test/pgtypeslib
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

TESTS = dt_test dt_test.c \
        dt_test2 dt_test2.c \
        num_test num_test.c \
        num_test2 num_test2.c \
        nan_test nan_test.c

all: $(TESTS)

