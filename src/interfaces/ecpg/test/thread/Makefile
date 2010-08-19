subdir = src/interfaces/ecpg/test/thread
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress


TESTS = thread_implicit thread_implicit.c \
        thread thread.c \
        prep prep.c \
        descriptor descriptor.c \
        alloc alloc.c

all: $(TESTS)
