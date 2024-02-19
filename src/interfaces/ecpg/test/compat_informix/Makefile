subdir = src/interfaces/ecpg/test/compat_informix
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

# Use special informix compatibility switch for all tests in this directory
ECPG += -C INFORMIX

LDFLAGS_INTERNAL += -L../../compatlib -lecpg_compat

TESTS = test_informix test_informix.c \
        test_informix2 test_informix2.c \
        dec_test dec_test.c \
        rfmtdate rfmtdate.c \
        rfmtlong rfmtlong.c \
        rnull rnull.c \
        sqlda sqlda.c \
        describe describe.c \
        charfuncs charfuncs.c \
        intoasc intoasc.c

all: $(TESTS)

rnull.c: rnull.pgc $(ECPG_TEST_DEPENDENCIES)
	$(ECPG) -r no_indicator -o $@ $<
