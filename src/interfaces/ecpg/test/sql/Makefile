subdir = src/interfaces/ecpg/test/sql
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

TESTS = array array.c \
	binary binary.c \
	code100 code100.c \
        copystdout copystdout.c \
	define define.c \
        desc desc.c \
        sqlda sqlda.c \
        describe describe.c \
        dyntest dyntest.c \
        dynalloc dynalloc.c \
        dynalloc2 dynalloc2.c \
        execute execute.c \
        fetch fetch.c \
        func func.c \
        indicators indicators.c \
	oldexec oldexec.c \
        parser parser.c \
        quote quote.c \
        show show.c \
        twophase twophase.c \
        insupd insupd.c

all: $(TESTS)

oldexec.c: oldexec.pgc $(ECPG_TEST_DEPENDENCIES)
	$(ECPG) -r questionmarks -o $@ $<
