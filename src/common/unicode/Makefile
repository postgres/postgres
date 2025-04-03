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

override CPPFLAGS := -DFRONTEND -I. $(CPPFLAGS)
LIBS += $(PTHREAD_LIBS)

LDFLAGS_INTERNAL += $(ICU_LIBS)
CPPFLAGS += $(ICU_CFLAGS)

# By default, do nothing.
all:

update-unicode: unicode_case_table.h unicode_category_table.h unicode_east_asian_fw_table.h unicode_nonspacing_table.h unicode_norm_hashfunc.h unicode_norm_table.h unicode_normprops_table.h unicode_version.h
	mv $^ $(top_srcdir)/src/include/common/
	$(MAKE) case-check
	$(MAKE) category-check
	$(MAKE) normalization-check

# These files are part of the Unicode Character Database. Download
# them on demand.  The dependency on Makefile.global is for
# UNICODE_VERSION.
CompositionExclusions.txt CaseFolding.txt DerivedCoreProperties.txt DerivedNormalizationProps.txt EastAsianWidth.txt NormalizationTest.txt PropList.txt SpecialCasing.txt UnicodeData.txt: $(top_builddir)/src/Makefile.global
	$(DOWNLOAD) https://www.unicode.org/Public/$(UNICODE_VERSION)/ucd/$(@F)

unicode_version.h: generate-unicode_version.pl
	$(PERL) $< --version $(UNICODE_VERSION)

unicode_case_table.h: generate-unicode_case_table.pl CaseFolding.txt SpecialCasing.txt UnicodeData.txt
	$(PERL) $<

unicode_category_table.h: generate-unicode_category_table.pl DerivedCoreProperties.txt PropList.txt UnicodeData.txt
	$(PERL) $<

# Generation of conversion tables used for string normalization with
# UTF-8 strings.
unicode_norm_hashfunc.h: unicode_norm_table.h

unicode_norm_table.h: generate-unicode_norm_table.pl UnicodeData.txt CompositionExclusions.txt
	$(PERL) $<

unicode_nonspacing_table.h: generate-unicode_nonspacing_table.pl UnicodeData.txt
	$(PERL) $^ >$@

unicode_east_asian_fw_table.h: generate-unicode_east_asian_fw_table.pl EastAsianWidth.txt
	$(PERL) $^ >$@

unicode_normprops_table.h: generate-unicode_normprops_table.pl DerivedNormalizationProps.txt
	$(PERL) $^ >$@

# Test suite
case-check: case_test
	./case_test

category-check: category_test
	./category_test

normalization-check: norm_test
	./norm_test

case_test: case_test.o ../unicode_case.o | submake-common

category_test: category_test.o ../unicode_category.o | submake-common

norm_test: norm_test.o ../unicode_norm.o | submake-common

norm_test.o: norm_test_table.h

.PHONY: submake-common

submake-common:
	$(MAKE) -C .. all

norm_test_table.h: generate-norm_test_table.pl NormalizationTest.txt
	perl $^ $@

.PHONY: normalization-check


clean:
	rm -f $(OBJS) case_test case_test.o category_test category_test.o norm_test norm_test.o

distclean: clean
	rm -f CompositionExclusions.txt CaseFolding.txt DerivedCoreProperties.txt DerivedNormalizationProps.txt EastAsianWidth.txt NormalizationTest.txt PropList.txt SpecialCasing.txt UnicodeData.txt norm_test_table.h unicode_case_table.h unicode_category_table.h unicode_norm_table.h
