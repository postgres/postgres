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

update-unicode: unicode_norm_table.h unicode_combining_table.h unicode_normprops_table.h
	mv $^ ../../../src/include/common/
	$(MAKE) normalization-check

# These files are part of the Unicode Character Database. Download
# them on demand.  The dependency on Makefile.global is for
# UNICODE_VERSION.
UnicodeData.txt DerivedNormalizationProps.txt CompositionExclusions.txt NormalizationTest.txt: $(top_builddir)/src/Makefile.global
	$(DOWNLOAD) https://www.unicode.org/Public/$(UNICODE_VERSION)/ucd/$(@F)

# Generation of conversion tables used for string normalization with
# UTF-8 strings.
unicode_norm_table.h: generate-unicode_norm_table.pl UnicodeData.txt CompositionExclusions.txt
	$(PERL) generate-unicode_norm_table.pl

unicode_combining_table.h: generate-unicode_combining_table.pl UnicodeData.txt
	$(PERL) $^ >$@

unicode_normprops_table.h: generate-unicode_normprops_table.pl DerivedNormalizationProps.txt
	$(PERL) $^ >$@

# Test suite
normalization-check: norm_test
	./norm_test

norm_test: norm_test.o ../unicode_norm.o | submake-common

norm_test.o: norm_test_table.h

.PHONY: submake-common

submake-common:
	$(MAKE) -C .. all

norm_test_table.h: generate-norm_test_table.pl NormalizationTest.txt
	perl generate-norm_test_table.pl NormalizationTest.txt $@

.PHONY: normalization-check


clean:
	rm -f $(OBJS) norm_test norm_test.o

distclean: clean
	rm -f UnicodeData.txt CompositionExclusions.txt NormalizationTest.txt norm_test_table.h unicode_norm_table.h

maintainer-clean: distclean
