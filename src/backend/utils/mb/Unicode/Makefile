#-------------------------------------------------------------------------
#
# Makefile for src/backend/utils/mb/Unicode
#
# Copyright (c) 2001-2016, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/Unicode
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ISO8859MAPS = iso8859_2_to_utf8.map utf8_to_iso8859_2.map \
	iso8859_3_to_utf8.map utf8_to_iso8859_3.map \
	iso8859_4_to_utf8.map utf8_to_iso8859_4.map \
	iso8859_5_to_utf8.map utf8_to_iso8859_5.map \
	iso8859_6_to_utf8.map utf8_to_iso8859_6.map \
	iso8859_7_to_utf8.map utf8_to_iso8859_7.map \
	iso8859_8_to_utf8.map utf8_to_iso8859_8.map \
	iso8859_9_to_utf8.map utf8_to_iso8859_9.map \
	iso8859_10_to_utf8.map utf8_to_iso8859_10.map \
	iso8859_13_to_utf8.map utf8_to_iso8859_13.map \
	iso8859_14_to_utf8.map utf8_to_iso8859_14.map \
	iso8859_15_to_utf8.map utf8_to_iso8859_15.map \
	iso8859_16_to_utf8.map utf8_to_iso8859_16.map

WINMAPS = win866_to_utf8.map utf8_to_win866.map \
	win874_to_utf8.map utf8_to_win874.map \
	win1250_to_utf8.map utf8_to_win1250.map \
	win1251_to_utf8.map utf8_to_win1251.map \
	win1252_to_utf8.map utf8_to_win1252.map \
	win1253_to_utf8.map utf8_to_win1253.map \
	win1254_to_utf8.map utf8_to_win1254.map \
	win1255_to_utf8.map utf8_to_win1255.map \
	win1256_to_utf8.map utf8_to_win1256.map \
	win1257_to_utf8.map utf8_to_win1257.map \
	win1258_to_utf8.map utf8_to_win1258.map

GENERICMAPS = $(ISO8859MAPS) $(WINMAPS) \
	johab_to_utf8.map utf8_to_johab.map \
	uhc_to_utf8.map utf8_to_uhc.map \
	gbk_to_utf8.map utf8_to_gbk.map \
	koi8r_to_utf8.map utf8_to_koi8r.map

SPECIALMAPS = euc_cn_to_utf8.map utf8_to_euc_cn.map \
	euc_jp_to_utf8.map utf8_to_euc_jp.map \
	euc_kr_to_utf8.map utf8_to_euc_kr.map \
	euc_tw_to_utf8.map utf8_to_euc_tw.map \
	sjis_to_utf8.map utf8_to_sjis.map \
	gb18030_to_utf8.map utf8_to_gb18030.map \
	big5_to_utf8.map utf8_to_big5.map

MAPS = $(GENERICMAPS) $(SPECIALMAPS)

ISO8859TEXTS = 8859-2.TXT 8859-3.TXT 8859-4.TXT 8859-5.TXT \
	8859-6.TXT 8859-7.TXT 8859-8.TXT 8859-9.TXT \
	8859-10.TXT 8859-13.TXT 8859-14.TXT 8859-15.TXT \
	8859-16.TXT

WINTEXTS = CP866.TXT CP874.TXT CP1250.TXT CP1251.TXT \
	CP1252.TXT CP1253.TXT CP1254.TXT CP1255.TXT \
	CP1256.TXT CP1257.TXT CP1258.TXT

GENERICTEXTS = $(ISO8859TEXTS) $(WINTEXTS) \
	KOI8-R.TXT CP936.TXT CP949.TXT JOHAB.TXT

all: $(MAPS)

$(GENERICMAPS) : $(GENERICTEXTS)
	$(PERL) $(srcdir)/UCS_to_most.pl

euc_jp_to_utf8.map utf8_to_euc_jp.map : JIS0201.TXT JIS0208.TXT JIS0212.TXT
	$(PERL) $(srcdir)/UCS_to_EUC_JP.pl

euc_cn_to_utf8.map utf8_to_euc_cn.map : GB2312.TXT
	$(PERL) $(srcdir)/UCS_to_EUC_CN.pl

euc_kr_to_utf8.map utf8_to_euc_kr.map : KSX1001.TXT
	$(PERL) $(srcdir)/UCS_to_EUC_KR.pl

euc_tw_to_utf8.map utf8_to_euc_tw.map : CNS11643.TXT
	$(PERL) $(srcdir)/UCS_to_EUC_TW.pl

sjis_to_utf8.map utf8_to_sjis.map : CP932.TXT
	$(PERL) $(srcdir)/UCS_to_SJIS.pl

gb18030_to_utf8.map  utf8_to_gb18030.map : gb-18030-2000.xml
	$(PERL) $(srcdir)/UCS_to_GB18030.pl

big5_to_utf8.map  utf8_to_big5.map : BIG5.TXT CP950.TXT
	$(PERL) $(srcdir)/UCS_to_BIG5.pl

distclean: clean
	rm -f $(TEXTS)

maintainer-clean: distclean
	rm -f $(MAPS)
