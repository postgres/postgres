subdir = src/interfaces/ecpg/test/sql
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

TESTS = define define.c \
        dyntest dyntest.c \
        code100 code100.c \
        desc desc.c \
        dynalloc dynalloc.c \
        dynalloc2 dynalloc2.c \
        func func.c \
        indicators indicators.c \
        quote quote.c \
        show show.c \
        copystdout copystdout.c

all: $(TESTS)

