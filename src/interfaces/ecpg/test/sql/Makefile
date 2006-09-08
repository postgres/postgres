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
        dyntest dyntest.c \
        dynalloc dynalloc.c \
        dynalloc2 dynalloc2.c \
        execute execute.c \
        fetch fetch.c \
        func func.c \
        indicators indicators.c \
        quote quote.c \
        show show.c \
        update update.c 

all: $(TESTS)

# bits needs the -c option for the "EXEC SQL TYPE" construct
bits.c: bits.pgc ../regression.h
	$(ECPG) -c -o $@ -I$(srcdir) $<

