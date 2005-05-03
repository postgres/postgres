#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb/conversion_procs
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/Makefile,v 1.9.2.2 2005/05/03 19:18:31 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/conversion_procs
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

SQLSCRIPT = conversion_create.sql

# This file can be placed as src/test/regress/conversion.sql
REGRESSION_SCRIPT = conversion.sql

DIRS = \
	ascii_and_mic cyrillic_and_mic euc_cn_and_mic euc_jp_and_sjis \
	euc_kr_and_mic euc_tw_and_big5 latin2_and_win1250 latin_and_mic \
	utf8_and_ascii utf8_and_big5 utf8_and_cyrillic utf8_and_euc_cn \
	utf8_and_euc_jp utf8_and_euc_kr utf8_and_euc_tw utf8_and_gb18030 \
	utf8_and_gbk utf8_and_iso8859 utf8_and_iso8859_1 utf8_and_johab \
	utf8_and_sjis utf8_and_tcvn utf8_and_uhc utf8_and_win1250 \
	utf8_and_win1256 utf8_and_win874

# conversion_name source_encoding destination_encoding function object
CONVERSIONS = \
		ascii_to_mic	SQL_ASCII MULE_INTERNAL ascii_to_mic ascii_and_mic \
		mic_to_ascii	MULE_INTERNAL SQL_ASCII mic_to_ascii ascii_and_mic \
		koi8_r_to_mic	KOI8R MULE_INTERNAL koi8r_to_mic cyrillic_and_mic \
		mic_to_koi8_r	MULE_INTERNAL KOI8R mic_to_koi8r cyrillic_and_mic \
		iso_8859_5_to_mic	ISO-8859-5 MULE_INTERNAL iso_to_mic cyrillic_and_mic \
		mic_to_iso_8859_5	MULE_INTERNAL ISO-8859-5 mic_to_iso cyrillic_and_mic \
		windows_1251_to_mic	WIN1251 MULE_INTERNAL win1251_to_mic cyrillic_and_mic \
		mic_to_windows_1251	MULE_INTERNAL WIN1251 mic_to_win1251 cyrillic_and_mic \
		windows_866_to_mic	ALT MULE_INTERNAL alt_to_mic cyrillic_and_mic \
		mic_to_windows_866	MULE_INTERNAL ALT mic_to_alt cyrillic_and_mic \
		koi8_r_to_windows_1251   KOI8R WIN1251 koi8r_to_win1251 cyrillic_and_mic \
		windows_1251_to_koi8_r   WIN1251 KOI8R win1251_to_koi8r cyrillic_and_mic \
		koi8_r_to_windows_866	KOI8R ALT koi8r_to_alt cyrillic_and_mic \
		windows_866_to_koi8_r	ALT KOI8R alt_to_koi8r cyrillic_and_mic \
		windows_866_to_windows_1251	ALT WIN1251 alt_to_win1251 cyrillic_and_mic \
		windows_1251_to_windows_866	WIN1251	ALT win1251_to_alt cyrillic_and_mic \
		iso_8859_5_to_koi8_r	ISO-8859-5 KOI8R iso_to_koi8r cyrillic_and_mic \
		koi8_r_to_iso_8859_5	KOI8R ISO-8859-5 koi8r_to_iso cyrillic_and_mic \
		iso_8859_5_to_windows_1251	ISO-8859-5 WIN1251 iso_to_win1251 cyrillic_and_mic \
		windows_1251_to_iso_8859_5	WIN1251 ISO-8859-5 win1251_to_iso cyrillic_and_mic \
		iso_8859_5_to_windows_866	ISO-8859-5 ALT iso_to_alt cyrillic_and_mic \
		windows_866_to_iso_8859_5	ALT ISO-8859-5 alt_to_iso cyrillic_and_mic \
		euc_cn_to_mic	EUC_CN MULE_INTERNAL euc_cn_to_mic euc_cn_and_mic \
		mic_to_euc_cn	MULE_INTERNAL EUC_CN mic_to_euc_cn euc_cn_and_mic \
		euc_jp_to_sjis	EUC_JP SJIS euc_jp_to_sjis euc_jp_and_sjis \
		sjis_to_euc_jp	SJIS EUC_JP sjis_to_euc_jp euc_jp_and_sjis \
		euc_jp_to_mic	EUC_JP MULE_INTERNAL euc_jp_to_mic euc_jp_and_sjis \
		sjis_to_mic	SJIS MULE_INTERNAL sjis_to_mic euc_jp_and_sjis \
		mic_to_euc_jp	MULE_INTERNAL EUC_JP mic_to_euc_jp euc_jp_and_sjis \
		mic_to_sjis	MULE_INTERNAL SJIS mic_to_sjis euc_jp_and_sjis \
		euc_kr_to_mic	EUC_KR MULE_INTERNAL euc_kr_to_mic euc_kr_and_mic \
		mic_to_euc_kr	MULE_INTERNAL EUC_KR mic_to_euc_kr euc_kr_and_mic \
		euc_tw_to_big5	EUC_TW BIG5 euc_tw_to_big5 euc_tw_and_big5 \
		big5_to_euc_tw	BIG5 EUC_TW big5_to_euc_tw euc_tw_and_big5 \
		euc_tw_to_mic	EUC_TW MULE_INTERNAL euc_tw_to_mic euc_tw_and_big5 \
		big5_to_mic	BIG5 MULE_INTERNAL big5_to_mic euc_tw_and_big5 \
		mic_to_euc_tw	MULE_INTERNAL EUC_TW mic_to_euc_tw euc_tw_and_big5 \
		mic_to_big5	MULE_INTERNAL BIG5 mic_to_big5 euc_tw_and_big5 \
		iso_8859_2_to_mic	LATIN2 MULE_INTERNAL latin2_to_mic latin2_and_win1250 \
		mic_to_iso_8859_2	MULE_INTERNAL LATIN2 mic_to_latin2 latin2_and_win1250 \
		windows_1250_to_mic	WIN1250 MULE_INTERNAL win1250_to_mic latin2_and_win1250 \
		mic_to_windows_1250	MULE_INTERNAL WIN1250 mic_to_win1250 latin2_and_win1250 \
		iso_8859_2_to_windows_1250  LATIN2 WIN1250 latin2_to_win1250 latin2_and_win1250 \
		windows_1250_to_iso_8859_2  WIN1250 LATIN2 win1250_to_latin2 latin2_and_win1250 \
		iso_8859_1_to_mic	LATIN1 MULE_INTERNAL latin1_to_mic latin_and_mic \
		mic_to_iso_8859_1	MULE_INTERNAL LATIN1 mic_to_latin1 latin_and_mic \
		iso_8859_3_to_mic	LATIN3 MULE_INTERNAL latin3_to_mic latin_and_mic \
		mic_to_iso_8859_3	MULE_INTERNAL LATIN3 mic_to_latin3 latin_and_mic \
		iso_8859_4_to_mic	LATIN4 MULE_INTERNAL latin4_to_mic latin_and_mic \
		mic_to_iso_8859_4	MULE_INTERNAL LATIN4 mic_to_latin4 latin_and_mic \
		ascii_to_utf_8 SQL_ASCII UNICODE ascii_to_utf8 utf8_and_ascii \
		utf_8_to_ascii UNICODE SQL_ASCII utf8_to_ascii utf8_and_ascii \
		big5_to_utf_8 BIG5 UNICODE big5_to_utf8 utf8_and_big5 \
		utf_8_to_big5 UNICODE BIG5 utf8_to_big5 utf8_and_big5 \
		utf_8_to_koi8_r	UNICODE KOI8R utf8_to_koi8r utf8_and_cyrillic \
		koi8_r_to_utf_8	KOI8R UNICODE koi8r_to_utf8 utf8_and_cyrillic \
		utf_8_to_windows_1251	UNICODE WIN1251 utf8_to_win1251 utf8_and_cyrillic \
		windows_1251_to_utf_8	WIN1251 UNICODE win1251_to_utf8 utf8_and_cyrillic \
		utf_8_to_windows_866	UNICODE ALT utf8_to_alt utf8_and_cyrillic \
		windows_866_to_utf_8	ALT UNICODE alt_to_utf8 utf8_and_cyrillic \
		euc_cn_to_utf_8 EUC_CN UNICODE euc_cn_to_utf8 utf8_and_euc_cn \
		utf_8_to_euc_cn UNICODE EUC_CN utf8_to_euc_cn utf8_and_euc_cn \
		euc_jp_to_utf_8 EUC_JP UNICODE euc_jp_to_utf8 utf8_and_euc_jp \
		utf_8_to_euc_jp UNICODE EUC_JP utf8_to_euc_jp utf8_and_euc_jp \
		euc_kr_to_utf_8 EUC_KR UNICODE euc_kr_to_utf8 utf8_and_euc_kr \
		utf_8_to_euc_kr UNICODE EUC_KR utf8_to_euc_kr utf8_and_euc_kr \
		euc_tw_to_utf_8 EUC_TW UNICODE euc_tw_to_utf8 utf8_and_euc_tw \
		utf_8_to_euc_tw UNICODE EUC_TW utf8_to_euc_tw utf8_and_euc_tw \
		gb18030_to_utf_8 GB18030 UNICODE gb18030_to_utf8 utf8_and_gb18030 \
		utf_8_to_gb18030 UNICODE GB18030 utf8_to_gb18030 utf8_and_gb18030 \
		gbk_to_utf_8 GBK UNICODE gbk_to_utf8 utf8_and_gbk \
		utf_8_to_gbk UNICODE GBK utf8_to_gbk utf8_and_gbk \
		utf_8_to_iso_8859_2 UNICODE LATIN2 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_2_to_utf_8 LATIN2 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_3 UNICODE LATIN3 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_3_to_utf_8 LATIN3 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_4 UNICODE LATIN4 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_4_to_utf_8 LATIN4 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_9 UNICODE LATIN5 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_9_to_utf_8 LATIN5 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_10 UNICODE LATIN6 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_10_to_utf_8 LATIN6 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_13 UNICODE LATIN7 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_13_to_utf_8 LATIN7 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_14 UNICODE LATIN8 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_14_to_utf_8 LATIN8 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_15 UNICODE LATIN9 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_15_to_utf_8 LATIN9 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_16 UNICODE LATIN10 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_16_to_utf_8 LATIN10 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_5 UNICODE ISO-8859-5 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_5_to_utf_8 ISO-8859-5 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_6 UNICODE ISO-8859-6 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_6_to_utf_8 ISO-8859-6 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_7 UNICODE ISO-8859-7 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_7_to_utf_8 ISO-8859-7 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf_8_to_iso_8859_8 UNICODE ISO-8859-8 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_8_to_utf_8 ISO-8859-8 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		iso_8859_1_to_utf_8 LATIN1 UNICODE iso8859_1_to_utf8 utf8_and_iso8859_1 \
		utf_8_to_iso_8859_1 UNICODE LATIN1 utf8_to_iso8859_1 utf8_and_iso8859_1 \
		johab_to_utf_8 JOHAB UNICODE johab_to_utf8 utf8_and_johab \
		utf_8_to_johab UNICODE JOHAB utf8_to_johab utf8_and_johab \
		sjis_to_utf_8 SJIS UNICODE sjis_to_utf8 utf8_and_sjis \
		utf_8_to_sjis UNICODE SJIS utf8_to_sjis utf8_and_sjis \
		tcvn_to_utf_8 TCVN UNICODE tcvn_to_utf8 utf8_and_tcvn \
		utf_8_to_tcvn UNICODE TCVN utf8_to_tcvn utf8_and_tcvn \
		uhc_to_utf_8 UHC UNICODE uhc_to_utf8 utf8_and_uhc \
		utf_8_to_uhc UNICODE UHC utf8_to_uhc utf8_and_uhc \
		utf_8_to_windows_1250  UNICODE WIN1250 utf_to_win1250 utf8_and_win1250 \
		windows_1250_to_utf_8  WIN1250 UNICODE win1250_to_utf utf8_and_win1250 \
		utf_8_to_windows_1256  UNICODE WIN1256 utf_to_win1256 utf8_and_win1256 \
		windows_1256_to_utf_8  WIN1256 UNICODE win1256_to_utf utf8_and_win1256 \
		utf_8_to_windows_874  UNICODE WIN874 utf_to_win874 utf8_and_win874 \
		windows_874_to_utf_8  WIN874 UNICODE win874_to_utf utf8_and_win874

all: $(SQLSCRIPT)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

$(SQLSCRIPT): Makefile
ifeq ($(enable_shared), yes)
	@set $(CONVERSIONS) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		name=$$1;shift; \
		se=$$1;shift; \
		de=$$1; shift; \
		func=$$1; shift; \
		obj=$$1; shift; \
		echo "-- $$se --> $$de"; \
		echo "CREATE OR REPLACE FUNCTION $$func (INTEGER, INTEGER, CSTRING, INTERNAL, INTEGER) RETURNS VOID AS '$$"libdir"/$$obj', '$$func' LANGUAGE 'c' STRICT;"; \
		echo "DROP CONVERSION pg_catalog.$$name;"; \
		echo "CREATE DEFAULT CONVERSION pg_catalog.$$name FOR '$$se' TO '$$de' FROM $$func;"; \
	done > $@
else
	echo "-- No conversion support, for lack of shared library support" > $@
endif

$(REGRESSION_SCRIPT): Makefile
	@cp regress_prolog $@; \
	set $(CONVERSIONS) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		name=$$1;shift; \
		se=$$1;shift; \
		de=$$1; shift; \
		func=$$1; shift; \
		obj=$$1; shift; \
		echo "-- $$se --> $$de"; \
		echo "SELECT CONVERT('foo' USING $$name);"; \
		echo "SELECT CONVERT('foo', '$$se', '$$de');"; \
	done >> $@; \
	cat regress_epilogue >> $@;

install: $(SQLSCRIPT) installdirs
	$(INSTALL_DATA) $(SQLSCRIPT) $(DESTDIR)$(datadir)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(datadir) $(DESTDIR)$(pkglibdir)

uninstall:
	rm -f $(DESTDIR)$(datadir)/$(SQLSCRIPT)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	rm -f $(SQLSCRIPT)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

