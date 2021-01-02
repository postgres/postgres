#-------------------------------------------------------------------------
#
# Makefile for backend/utils/mb/conversion_procs
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/utils/mb/conversion_procs/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb/conversion_procs
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
	cyrillic_and_mic euc_cn_and_mic euc_jp_and_sjis \
	euc_kr_and_mic euc_tw_and_big5 latin2_and_win1250 latin_and_mic \
	utf8_and_big5 utf8_and_cyrillic utf8_and_euc_cn \
	utf8_and_euc_jp utf8_and_euc_kr utf8_and_euc_tw utf8_and_gb18030 \
	utf8_and_gbk utf8_and_iso8859 utf8_and_iso8859_1 utf8_and_johab \
	utf8_and_sjis utf8_and_win utf8_and_uhc \
	utf8_and_euc2004 utf8_and_sjis2004 euc2004_sjis2004

$(recurse)
