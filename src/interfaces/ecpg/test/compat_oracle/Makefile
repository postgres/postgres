subdir = src/interfaces/ecpg/test/compat_oracle
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/$(subdir)/../Makefile.regress

# Use special oracle compatibility switch for all tests in this directory
ECPG += -C ORACLE

TESTS = char_array char_array.c

all: $(TESTS)
