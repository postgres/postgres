#-------------------------------------------------------------------------
#
# Makefile for src/backend/utils/mb/Unicode
#
# Copyright (c) 2001-2003, PostgreSQL Global Development Group
#
# $Header: /cvsroot/pgsql/src/backend/utils/mb/Unicode/Makefile,v 1.5 2003/08/04 23:59:39 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/Unicode
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

ISO8859MAPS=iso8859_2_to_utf8.map iso8859_3_to_utf8.map \
	iso8859_4_to_utf8.map iso8859_5_to_utf8.map \
	utf8_to_iso8859_2.map utf8_to_iso8859_3.map \
	utf8_to_iso8859_4.map utf8_to_iso8859_5.map 


CYRILLICMAPS=koi8r_to_utf8.map win1251_to_utf8.map alt_to_utf8.map\
             utf8_to_koi8r.map  utf8_to_win1251.map utf8_to_alt.map

MAPS= $(ISO8859MAPS) $(CYRILLICMAPS)\
	big5_to_utf8.map euc_cn_to_utf8.map euc_jp_to_utf8.map \
	euc_kr_to_utf8.map euc_tw_to_utf8.map sjis_to_utf8.map \
	utf8_to_big5.map utf8_to_euc_cn.map utf8_to_euc_jp.map \
	utf8_to_euc_kr.map utf8_to_euc_tw.map utf8_to_iso8859_2.map \
	utf8_to_sjis.map gb18030_to_utf8.map utf8_to_gb18030.map

ISO8859TEXTS= 8859-2.TXT 8859-3.TXT 8859-4.TXT 8859-5.TXT
CYRILLICTEXTS=cp866.txt cp1251.txt koi8-r.txt

TEXTS=$(ISO8859TEXTS) $(CYRILLICTEXTS) \
	BIG5.TXT CNS11643.TXT GB2312.TXT \
	JIS0201.TXT JIS0208.TXT JIS0212.TXT \
	OLD5601.TXT SHIFTJIS.TXT ISO10646-GB18030.TXT

all: $(MAPS)

$(ISO8859MAPS) : $(ISO8859TEXTS)
	./UCS_to_8859.pl
$(CYRILLICMAPS) : $(CYRILLICTEXTS)
	./UCS_to_cyrillic.pl

euc_jp_to_utf8.map utf8_to_euc_jp.map : JIS0201.TXT JIS0208.TXT JIS0212.TXT
	./UCS_to_EUC_JP.pl

euc_cn_to_utf8.map utf8_to_euc_cn.map : GB2312.TXT
	./UCS_to_EUC_CN.pl

euc_kr_to_utf8.map utf8_to_euc_kr.map : OLD5601.TXT
	./UCS_to_EUC_KR.pl

euc_tw_to_utf8.map utf8_to_euc_tw.map : CNS11643.TXT
	./UCS_to_EUC_TW.pl

sjis_to_utf8.map utf8_to_sjis.map : SHIFTJIS.TXT
	./UCS_to_SJIS.pl

big5_to_utf8.map utf8_to_big5.map : BIG5.TXT
	./UCS_to_BIG5.pl

gb18030_to_utf8.map  utf8_to_gb18030.map : ISO10646-GB18030.TXT
	./UCS_to_GB18030.pl
clean:
	rm -f $(MAPS)

distclean: clean
	rm -f $(TEXTS)
