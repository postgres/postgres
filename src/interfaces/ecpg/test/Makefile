# $Header: /cvsroot/pgsql/src/interfaces/ecpg/test/Makefile,v 1.32 2001/11/14 11:11:49 meskes Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir)/../include $(CPPFLAGS)

ECPG = ../preproc/ecpg -I$(srcdir)/../include

TESTS = test1 test2 test3 test4 perftest dyntest dyntest2 test_notice test_code100 test_init testdynalloc

all: $(TESTS)

%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L../lib -L../../libpq $^ $(LIBS) -lecpg -lpq -o $@

%.c: %.pgc
	$(ECPG) $<

clean:
	rm -f $(TESTS) $(TESTS:%=%.o) $(TESTS:%=%.c) log
