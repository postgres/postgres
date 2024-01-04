#-------------------------------------------------------------------------
#
# Makefile for src/backend/utils/mb/Unicode
#
# Copyright (c) 2001-2024, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/Unicode
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global


# Define a rule to create the map files from downloaded text input
# files using a script.  Arguments:
#
# 1: encoding name used in output files (lower case)
# 2: script name
# 3: input text files
# 4: argument to pass to script (optional)
#
# We also collect all the input and output files in variables to
# define the build and clean rules below.
#
# Note that while each script call produces two output files, to be
# parallel-make safe we need to split this into two rules.  (See notes
# in src/backend/parser/Makefile about rules with multiple outputs.)
#
define map_rule
MAPS += $(1)_to_utf8.map utf8_to_$(1).map
ALL_TEXTS += $(3)

$(1)_to_utf8.map: $(2) $(3)
	$(PERL) -I $$(srcdir) $$< $(4)

utf8_to_$(1).map: $(1)_to_utf8.map
	@touch $$@
endef

$(foreach n,2 3 4 5 6 7 8 9 10 13 14 15 16,$(eval $(call map_rule,iso8859_$(n),UCS_to_most.pl,8859-$(n).TXT,ISO8859_$(n))))

$(foreach n,866 874 1250 1251 1252 1253 1254 1255 1256 1257 1258,$(eval $(call map_rule,win$(n),UCS_to_most.pl,CP$(n).TXT,WIN$(n))))

$(eval $(call map_rule,koi8r,UCS_to_most.pl,KOI8-R.TXT,KOI8R))
$(eval $(call map_rule,koi8u,UCS_to_most.pl,KOI8-U.TXT,KOI8U))
$(eval $(call map_rule,gbk,UCS_to_most.pl,CP936.TXT,GBK))

$(eval $(call map_rule,johab,UCS_to_JOHAB.pl,JOHAB.TXT))
$(eval $(call map_rule,uhc,UCS_to_UHC.pl,windows-949-2000.xml))
$(eval $(call map_rule,euc_jp,UCS_to_EUC_JP.pl,CP932.TXT JIS0212.TXT))
$(eval $(call map_rule,euc_cn,UCS_to_EUC_CN.pl,gb-18030-2000.xml))
$(eval $(call map_rule,euc_kr,UCS_to_EUC_KR.pl,KSX1001.TXT))
$(eval $(call map_rule,euc_tw,UCS_to_EUC_TW.pl,CNS11643.TXT))
$(eval $(call map_rule,sjis,UCS_to_SJIS.pl,CP932.TXT))
$(eval $(call map_rule,gb18030,UCS_to_GB18030.pl,gb-18030-2000.xml))
$(eval $(call map_rule,big5,UCS_to_BIG5.pl,CP950.TXT BIG5.TXT CP950.TXT))
$(eval $(call map_rule,euc_jis_2004,UCS_to_EUC_JIS_2004.pl,euc-jis-2004-std.txt))
$(eval $(call map_rule,shift_jis_2004,UCS_to_SHIFT_JIS_2004.pl,sjis-0213-2004-std.txt))

# remove duplicates
TEXTS = $(sort $(ALL_TEXTS))

all: $(MAPS)

distclean: clean
	rm -f $(TEXTS)
	rm -f $(MAPS)


BIG5.TXT CNS11643.TXT:
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/OTHER/$(@F)

euc-jis-2004-std.txt sjis-0213-2004-std.txt:
	$(DOWNLOAD) http://x0213.org/codetable/$(@F)

gb-18030-2000.xml windows-949-2000.xml:
	$(DOWNLOAD) https://raw.githubusercontent.com/unicode-org/icu-data/master/charset/data/xml/$(@F)

GB2312.TXT:
	$(DOWNLOAD) 'http://trac.greenstone.org/browser/trunk/gsdl/unicode/MAPPINGS/EASTASIA/GB/GB2312.TXT?rev=1842&format=txt'

JIS0212.TXT:
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/JIS/$(@F)

JOHAB.TXT KSX1001.TXT:
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/OBSOLETE/EASTASIA/KSC/$(@F)

KOI8-R.TXT KOI8-U.TXT:
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/VENDORS/MISC/$(@F)

$(filter 8859-%.TXT,$(TEXTS)):
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/ISO8859/$(@F)

$(filter CP9%.TXT CP12%.TXT,$(TEXTS)):
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/$(@F)

$(filter CP8%.TXT,$(TEXTS)):
	$(DOWNLOAD) https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/$(@F)
