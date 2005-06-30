# $PostgreSQL: pgsql/src/interfaces/ecpg/test/Makefile,v 1.50 2005/06/30 07:01:57 neilc Exp $

subdir = src/interfaces/ecpg/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(srcdir)/../include -I$(libpq_srcdir) $(CPPFLAGS)
override CFLAGS += $(PTHREAD_CFLAGS)

ECPG = ../preproc/ecpg -I$(srcdir)/../include

TESTS = test1 test2 test3 test4 test5 perftest dyntest dyntest2 test_notice \
	test_code100 test_init testdynalloc num_test dt_test test_informix \
	test_desc
ifeq ($(enable_thread_safety), yes)
TESTS += test_thread test_thread_implicit
endif

all: $(TESTS)

%: %.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L../ecpglib -L ../pgtypeslib -L../../libpq $^ $(LIBS) -lpgtypes -lecpg -lpq $(PTHREAD_LIBS) -o $@

test_informix: test_informix.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L../compatlib -L../ecpglib -L ../pgtypeslib -L../../libpq $^ $(LIBS) -lpgtypes -lecpg -lecpg_compat -lpq $(PTHREAD_LIBS) -o $@

%.c: %.pgc
	$(ECPG) -o $@ -I$(srcdir) $<

test_informix.c: test_informix.pgc
	$(ECPG) -o $@ -C INFORMIX -r no_indicator $<

clean:
	rm -f $(TESTS) $(TESTS:%=%.o) $(TESTS:%=%.c) log
