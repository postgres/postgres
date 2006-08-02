subdir = src/interfaces/ecpg/test/complex
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include ../Makefile.regress


TESTS = test1 test1.c \
        test2 test2.c \
        test3 test3.c \
        test4 test4.c \
        test5 test5.c

all: $(TESTS)

# test4 needs the -c option for the "EXEC SQL TYPE" construct
test4.c: test4.pgc ../regression.h
	$(ECPG) -c -o $@ -I$(srcdir) $<
