# $Header: /cvsroot/pgsql/src/interfaces/ecpg/test/Makefile,v 1.36 2003/04/08 17:09:51 tgl Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir)/../include $(CPPFLAGS)

ECPG = ../preproc/ecpg -I$(srcdir)/../include

TESTS = test1 test2 test3 test4 perftest dyntest dyntest2 test_notice test_code100 test_init testdynalloc num_test dt_test

all: $(TESTS)

%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L../ecpglib -L ../pgtypeslib -L../../libpq $^ $(LIBS) -lpgtypes -lecpg -lpq -o $@

%.c: %.pgc
	$(ECPG) $<

clean:
	rm -f $(TESTS) $(TESTS:%=%.o) $(TESTS:%=%.c) log
