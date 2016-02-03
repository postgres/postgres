#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb/conversion_procs
#
# IDENTIFICATION
#    src/backend/utils/mb/conversion_procs/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/conversion_procs
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

SQLSCRIPT = conversion_create.sql

SUBDIRS = \
	ascii_and_mic cyrillic_and_mic euc_cn_and_mic euc_jp_and_sjis \
	euc_kr_and_mic euc_tw_and_big5 latin2_and_win1250 latin_and_mic \
	utf8_and_ascii utf8_and_big5 utf8_and_cyrillic utf8_and_euc_cn \
	utf8_and_euc_jp utf8_and_euc_kr utf8_and_euc_tw utf8_and_gb18030 \
	utf8_and_gbk utf8_and_iso8859 utf8_and_iso8859_1 utf8_and_johab \
	utf8_and_sjis utf8_and_win utf8_and_uhc \
	utf8_and_euc2004 utf8_and_sjis2004 euc2004_sjis2004

$(recurse)

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
		windows_866_to_mic	WIN866 MULE_INTERNAL win866_to_mic cyrillic_and_mic \
		mic_to_windows_866	MULE_INTERNAL WIN866 mic_to_win866 cyrillic_and_mic \
		koi8_r_to_windows_1251   KOI8R WIN1251 koi8r_to_win1251 cyrillic_and_mic \
		windows_1251_to_koi8_r   WIN1251 KOI8R win1251_to_koi8r cyrillic_and_mic \
		koi8_r_to_windows_866	KOI8R WIN866 koi8r_to_win866 cyrillic_and_mic \
		windows_866_to_koi8_r	WIN866 KOI8R win866_to_koi8r cyrillic_and_mic \
		windows_866_to_windows_1251	WIN866 WIN1251 win866_to_win1251 cyrillic_and_mic \
		windows_1251_to_windows_866	WIN1251	WIN866 win1251_to_win866 cyrillic_and_mic \
		iso_8859_5_to_koi8_r	ISO-8859-5 KOI8R iso_to_koi8r cyrillic_and_mic \
		koi8_r_to_iso_8859_5	KOI8R ISO-8859-5 koi8r_to_iso cyrillic_and_mic \
		iso_8859_5_to_windows_1251	ISO-8859-5 WIN1251 iso_to_win1251 cyrillic_and_mic \
		windows_1251_to_iso_8859_5	WIN1251 ISO-8859-5 win1251_to_iso cyrillic_and_mic \
		iso_8859_5_to_windows_866	ISO-8859-5 WIN866 iso_to_win866 cyrillic_and_mic \
		windows_866_to_iso_8859_5	WIN866 ISO-8859-5 win866_to_iso cyrillic_and_mic \
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
		ascii_to_utf8 SQL_ASCII UTF8 ascii_to_utf8 utf8_and_ascii \
		utf8_to_ascii UTF8 SQL_ASCII utf8_to_ascii utf8_and_ascii \
		big5_to_utf8 BIG5 UTF8 big5_to_utf8 utf8_and_big5 \
		utf8_to_big5 UTF8 BIG5 utf8_to_big5 utf8_and_big5 \
		utf8_to_koi8_r	UTF8 KOI8R utf8_to_koi8r utf8_and_cyrillic \
		koi8_r_to_utf8	KOI8R UTF8 koi8r_to_utf8 utf8_and_cyrillic \
		utf8_to_koi8_u	UTF8 KOI8U utf8_to_koi8u utf8_and_cyrillic \
		koi8_u_to_utf8	KOI8U UTF8 koi8u_to_utf8 utf8_and_cyrillic \
		utf8_to_windows_866 UTF8 WIN866 utf8_to_win utf8_and_win \
		windows_866_to_utf8 WIN866 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_874 UTF8 WIN874 utf8_to_win utf8_and_win \
		windows_874_to_utf8 WIN874 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1250 UTF8 WIN1250 utf8_to_win utf8_and_win \
		windows_1250_to_utf8 WIN1250 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1251 UTF8 WIN1251 utf8_to_win utf8_and_win \
		windows_1251_to_utf8 WIN1251 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1252 UTF8 WIN1252 utf8_to_win utf8_and_win \
		windows_1252_to_utf8 WIN1252 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1253 UTF8 WIN1253 utf8_to_win utf8_and_win \
		windows_1253_to_utf8 WIN1253 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1254 UTF8 WIN1254 utf8_to_win utf8_and_win \
		windows_1254_to_utf8 WIN1254 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1255 UTF8 WIN1255 utf8_to_win utf8_and_win \
		windows_1255_to_utf8 WIN1255 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1256 UTF8 WIN1256 utf8_to_win utf8_and_win \
		windows_1256_to_utf8 WIN1256 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1257 UTF8 WIN1257 utf8_to_win utf8_and_win \
		windows_1257_to_utf8 WIN1257 UTF8 win_to_utf8 utf8_and_win \
		utf8_to_windows_1258 UTF8 WIN1258 utf8_to_win utf8_and_win \
		windows_1258_to_utf8 WIN1258 UTF8 win_to_utf8 utf8_and_win \
		euc_cn_to_utf8 EUC_CN UTF8 euc_cn_to_utf8 utf8_and_euc_cn \
		utf8_to_euc_cn UTF8 EUC_CN utf8_to_euc_cn utf8_and_euc_cn \
		euc_jp_to_utf8 EUC_JP UTF8 euc_jp_to_utf8 utf8_and_euc_jp \
		utf8_to_euc_jp UTF8 EUC_JP utf8_to_euc_jp utf8_and_euc_jp \
		euc_kr_to_utf8 EUC_KR UTF8 euc_kr_to_utf8 utf8_and_euc_kr \
		utf8_to_euc_kr UTF8 EUC_KR utf8_to_euc_kr utf8_and_euc_kr \
		euc_tw_to_utf8 EUC_TW UTF8 euc_tw_to_utf8 utf8_and_euc_tw \
		utf8_to_euc_tw UTF8 EUC_TW utf8_to_euc_tw utf8_and_euc_tw \
		gb18030_to_utf8 GB18030 UTF8 gb18030_to_utf8 utf8_and_gb18030 \
		utf8_to_gb18030 UTF8 GB18030 utf8_to_gb18030 utf8_and_gb18030 \
		gbk_to_utf8 GBK UTF8 gbk_to_utf8 utf8_and_gbk \
		utf8_to_gbk UTF8 GBK utf8_to_gbk utf8_and_gbk \
		utf8_to_iso_8859_2 UTF8 LATIN2 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_2_to_utf8 LATIN2 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_3 UTF8 LATIN3 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_3_to_utf8 LATIN3 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_4 UTF8 LATIN4 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_4_to_utf8 LATIN4 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_9 UTF8 LATIN5 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_9_to_utf8 LATIN5 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_10 UTF8 LATIN6 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_10_to_utf8 LATIN6 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_13 UTF8 LATIN7 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_13_to_utf8 LATIN7 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_14 UTF8 LATIN8 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_14_to_utf8 LATIN8 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_15 UTF8 LATIN9 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_15_to_utf8 LATIN9 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_16 UTF8 LATIN10 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_16_to_utf8 LATIN10 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_5 UTF8 ISO-8859-5 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_5_to_utf8 ISO-8859-5 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_6 UTF8 ISO-8859-6 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_6_to_utf8 ISO-8859-6 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_7 UTF8 ISO-8859-7 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_7_to_utf8 ISO-8859-7 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso_8859_8 UTF8 ISO-8859-8 utf8_to_iso8859 utf8_and_iso8859 \
		iso_8859_8_to_utf8 ISO-8859-8 UTF8 iso8859_to_utf8 utf8_and_iso8859 \
		iso_8859_1_to_utf8 LATIN1 UTF8 iso8859_1_to_utf8 utf8_and_iso8859_1 \
		utf8_to_iso_8859_1 UTF8 LATIN1 utf8_to_iso8859_1 utf8_and_iso8859_1 \
		johab_to_utf8 JOHAB UTF8 johab_to_utf8 utf8_and_johab \
		utf8_to_johab UTF8 JOHAB utf8_to_johab utf8_and_johab \
		sjis_to_utf8 SJIS UTF8 sjis_to_utf8 utf8_and_sjis \
		utf8_to_sjis UTF8 SJIS utf8_to_sjis utf8_and_sjis \
		uhc_to_utf8 UHC UTF8 uhc_to_utf8 utf8_and_uhc \
		utf8_to_uhc UTF8 UHC utf8_to_uhc utf8_and_uhc \
		euc_jis_2004_to_utf8 EUC_JIS_2004 UTF8 euc_jis_2004_to_utf8 utf8_and_euc2004 \
		utf8_to_euc_jis_2004 UTF8 EUC_JIS_2004 utf8_to_euc_jis_2004 utf8_and_euc2004 \
		shift_jis_2004_to_utf8 SHIFT_JIS_2004 UTF8 shift_jis_2004_to_utf8 utf8_and_sjis2004 \
		utf8_to_shift_jis_2004 UTF8 SHIFT_JIS_2004 utf8_to_shift_jis_2004 utf8_and_sjis2004 \
		euc_jis_2004_to_shift_jis_2004 EUC_JIS_2004 SHIFT_JIS_2004 euc_jis_2004_to_shift_jis_2004 euc2004_sjis2004 \
		shift_jis_2004_to_euc_jis_2004 SHIFT_JIS_2004 EUC_JIS_2004 shift_jis_2004_to_euc_jis_2004 euc2004_sjis2004

all: $(SQLSCRIPT)

$(SQLSCRIPT): Makefile
	@set -e; \
	set $(CONVERSIONS) ; \
	while [ "$$#" -gt 0 ] ; \
	do \
		name=$$1;shift; \
		se=$$1;shift; \
		de=$$1; shift; \
		func=$$1; shift; \
		obj=$$1; shift; \
		echo "-- $$se --> $$de"; \
		echo "CREATE OR REPLACE FUNCTION $$func (INTEGER, INTEGER, CSTRING, INTERNAL, INTEGER) RETURNS VOID AS '$$"libdir"/$$obj', '$$func' LANGUAGE C STRICT;"; \
	        echo "COMMENT ON FUNCTION $$func(INTEGER, INTEGER, CSTRING, INTERNAL, INTEGER) IS 'internal conversion function for $$se to $$de';"; \
		echo "DROP CONVERSION pg_catalog.$$name;"; \
		echo "CREATE DEFAULT CONVERSION pg_catalog.$$name FOR '$$se' TO '$$de' FROM $$func;"; \
	        echo "COMMENT ON CONVERSION pg_catalog.$$name IS 'conversion for $$se to $$de';"; \
		echo; \
	done > $@

install: $(SQLSCRIPT) installdirs
	$(INSTALL_DATA) $(SQLSCRIPT) '$(DESTDIR)$(datadir)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(datadir)' '$(DESTDIR)$(pkglibdir)'

uninstall:
	rm -f '$(DESTDIR)$(datadir)/$(SQLSCRIPT)'

clean distclean maintainer-clean:
	rm -f $(SQLSCRIPT)
