# $PostgreSQL: pgsql/src/interfaces/ecpg/test/Makefile,v 1.42 2003/11/29 19:52:09 pgsql Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir)/../include -I$(libpq_srcdir) $(CPPFLAGS) $(THREAD_CPPFLAGS)

ECPG = ../preproc/ecpg -I$(srcdir)/../include

TESTS = test1 test2 test3 test4 perftest dyntest dyntest2 test_notice test_code100 test_init testdynalloc num_test dt_test test_informix test_thread

all: $(TESTS)

%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L../ecpglib -L ../pgtypeslib -L../../libpq $^ $(LIBS) -lpgtypes -lecpg -lpq $(THREAD_LIBS) -o $@

test_informix: test_informix.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L../compatlib -L../ecpglib -L ../pgtypeslib -L../../libpq $^ $(LIBS) -lpgtypes -lecpg -lecpg_compat -lpq $(THREAD_LIBS) -o $@
	
%.c: %.pgc
	$(ECPG) $<

test_informix.c: test_informix.pgc
	$(ECPG) -C INFORMIX -r no_indicator $<
clean:
	rm -f $(TESTS) $(TESTS:%=%.o) $(TESTS:%=%.c) log
	
