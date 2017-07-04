#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for src/common/unicode
#
# IDENTIFICATION
#    src/common/unicode/Makefile
#
#-------------------------------------------------------------------------

subdir = src/common/unicode
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

# By default, do nothing.
all:

DOWNLOAD = wget -O $@ --no-use-server-timestamps

# These files are part of the Unicode Character Database. Download
# them on demand.
UnicodeData.txt CompositionExclusions.txt NormalizationTest.txt:
	$(DOWNLOAD) http://unicode.org/Public/UNIDATA/$(@F)

# Generation of conversion tables used for string normalization with
# UTF-8 strings.
unicode_norm_table.h: generate-unicode_norm_table.pl UnicodeData.txt CompositionExclusions.txt
	$(PERL) generate-unicode_norm_table.pl

# Test suite
normalization-check: norm_test
	./norm_test

norm_test: norm_test.o ../unicode_norm.o

norm_test.o: norm_test_table.h

norm_test_table.h: generate-norm_test_table.pl NormalizationTest.txt
	perl generate-norm_test_table.pl NormalizationTest.txt $@

.PHONY: normalization-check


clean:
	rm -f $(OBJS) norm_test norm_test.o

distclean: clean
	rm -f UnicodeData.txt CompositionExclusions.txt NormalizationTest.txt norm_test_table.h unicode_norm_table.h

maintainer-clean: distclean
