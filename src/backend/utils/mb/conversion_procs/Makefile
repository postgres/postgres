#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb/conversion_procs
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/Makefile,v 1.1 2002/07/16 09:25:04 ishii Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/conversion_procs
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

SQLSCRIPT = conversion_create.sql

DIRS = \
	utf8_and_ascii utf8_and_iso8859_1 \
	utf8_and_euc_jp utf8_and_euc_kr utf8_and_euc_cn utf8_and_euc_tw \
	utf8_and_sjis utf8_and_big5 utf8_and_gbk utf8_and_gb18030 \
	utf8_and_uhc utf8_and_johab utf8_and_tcvn utf8_and_iso8859 \
	euc_jp_and_sjis euc_tw_and_big5

# conversion_name source_encoding destination_encoding function object
$(SQLSCRIPT): Makefile
	@set \
		utf8_to_ascii UNICODE SQL_ASCII utf8_to_ascii utf8_and_ascii \
		ascii_to_utf8 SQL_ASCII UNICODE ascii_to_utf8 utf8_and_ascii \
		utf8_to_iso8859_1 UNICODE LATIN1 utf8_to_iso8859_1 utf8_and_iso8859_1 \
		iso8859_1_to_utf8 LATIN1 UNICODE iso8859_1_to_utf8 utf8_and_iso8859_1 \
		euc_jp_to_utf8 EUC_JP UNICODE euc_jp_to_utf8 utf8_and_euc_jp \
		utf8_to_euc_jp UNICODE EUC_JP utf8_to_euc_jp utf8_and_euc_jp \
		euc_kr_to_utf8 EUC_KR UNICODE euc_kr_to_utf8 utf8_and_euc_kr \
		utf8_to_euc_kr UNICODE EUC_KR utf8_to_euc_kr utf8_and_euc_kr \
		euc_cn_to_utf8 EUC_CN UNICODE euc_cn_to_utf8 utf8_and_euc_cn \
		utf8_to_euc_cn UNICODE EUC_CN utf8_to_euc_cn utf8_and_euc_cn \
		euc_tw_to_utf8 EUC_TW UNICODE euc_tw_to_utf8 utf8_and_euc_tw \
		utf8_to_euc_tw UNICODE EUC_TW utf8_to_euc_tw utf8_and_euc_tw \
		sjis_to_utf8 SJIS UNICODE sjis_to_utf8 utf8_and_sjis \
		utf8_to_sjis UNICODE SJIS utf8_to_sjis utf8_and_sjis \
		big5_to_utf8 BIG5 UNICODE big5_to_utf8 utf8_and_big5 \
		utf8_to_big5 UNICODE BIG5 utf8_to_big5 utf8_and_big5 \
		gbk_to_utf8 GBK UNICODE gbk_to_utf8 utf8_and_gbk \
		utf8_to_gbk UNICODE GBK utf8_to_gbk utf8_and_gbk \
		gb18030_to_utf8 GB18030 UNICODE gb18030_to_utf8 utf8_and_gb18030 \
		utf8_to_gb18030 UNICODE GB18030 utf8_to_gb18030 utf8_and_gb18030 \
		uhc_to_utf8 UHC UNICODE uhc_to_utf8 utf8_and_uhc \
		utf8_to_uhc UNICODE UHC utf8_to_uhc utf8_and_uhc \
		johab_to_utf8 JOHAB UNICODE johab_to_utf8 utf8_and_johab \
		utf8_to_johab UNICODE JOHAB utf8_to_johab utf8_and_johab \
		tcvn_to_utf8 TCVN UNICODE tcvn_to_utf8 utf8_and_tcvn \
		utf8_to_tcvn UNICODE TCVN utf8_to_tcvn utf8_and_tcvn \
		utf8_to_iso8859_2 UNICODE LATIN2 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_2_to_utf8 LATIN2 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_3 UNICODE LATIN3 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_3_to_utf8 LATIN3 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_4 UNICODE LATIN4 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_4_to_utf8 LATIN4 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_9 UNICODE LATIN5 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_9_to_utf8 LATIN5 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_10 UNICODE LATIN6 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_10_to_utf8 LATIN6 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_13 UNICODE LATIN7 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_13_to_utf8 LATIN7 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_14 UNICODE LATIN8 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_14_to_utf8 LATIN8 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_15 UNICODE LATIN9 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_15_to_utf8 LATIN9 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_16 UNICODE LATIN10 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_16_to_utf8 LATIN10 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_5 UNICODE ISO-8859-5 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_5_to_utf8 ISO-8859-5 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_6 UNICODE ISO-8859-6 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_6_to_utf8 ISO-8859-6 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_7 UNICODE ISO-8859-7 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_7_to_utf8 ISO-8859-7 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		utf8_to_iso8859_8 UNICODE ISO-8859-8 utf8_to_iso8859 utf8_and_iso8859 \
		iso8859_8_to_utf8 ISO-8859-8 UNICODE iso8859_to_utf8 utf8_and_iso8859 \
		euc_jp_to_sjis	EUC_JP SJIS euc_jp_to_sjis euc_jp_and_sjis \
		sjis_to_euc_jp	SJIS EUC_JP sjis_to_euc_jp euc_jp_and_sjis \
		euc_jp_to_mic	EUC_JP MULE_INTERNAL euc_jp_to_mic euc_jp_and_sjis \
		sjis_to_mic	SJIS MULE_INTERNAL sjis_to_mic euc_jp_and_sjis \
		mic_to_euc_jp	MULE_INTERNAL EUC_JP mic_to_euc_jp euc_jp_and_sjis \
		mic_to_sjis	MULE_INTERNAL SJIS mic_to_sjis euc_jp_and_sjis \
		euc_tw_to_big5	EUC_TW BIG5 euc_tw_to_big5 euc_tw_and_big5 \
		big5_to_euc_tw	BIG5 EUC_TW big5_to_euc_tw euc_tw_and_big5 \
		euc_tw_to_mic	EUC_TW MULE_INTERNAL euc_tw_to_mic euc_tw_and_big5 \
		big5_to_mic	BIG5 MULE_INTERNAL big5_to_mic euc_tw_and_big5 \
		mic_to_euc_tw	MULE_INTERNAL EUC_TW mic_to_euc_tw euc_tw_and_big5 \
		mic_to_big5	MULE_INTERNAL BIG5 mic_to_big5 euc_tw_and_big5 \
	; \
	while [ "$$#" -gt 0 ] ; \
	do \
		name=$$1;shift; \
		se=$$1;shift; \
		de=$$1; shift; \
		func=$$1; shift; \
		obj=$$1; shift; \
		echo "-- $$se --> $$de"; \
		echo "CREATE OR REPLACE FUNCTION $$func (INTEGER, INTEGER, OPAQUE, OPAQUE, INTEGER) RETURNS INTEGER AS '$$"libdir"/$$obj', '$$func' LANGUAGE 'c';"; \
		echo "DROP CONVERSION pg_catalog.$$name;"; \
		echo "CREATE DEFAULT CONVERSION pg_catalog.$$name FOR '$$se' TO '$$de' FROM $$func;"; \
	done > $@

install:
	$(INSTALL_DATA) $(SQLSCRIPT) $(datadir)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

all: $(SQLSCRIPT)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean:
	$(RM) $(SQLSCRIPT)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

distclean maintainer-clean:
	$(RM) $(SQLSCRIPT)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done
