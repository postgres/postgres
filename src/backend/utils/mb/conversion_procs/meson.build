# Copyright (c) 2022-2024, PostgreSQL Global Development Group

encodings = {
  'cyrillic_and_mic': ['cyrillic_and_mic/cyrillic_and_mic.c'],
  'euc2004_sjis2004': ['euc2004_sjis2004/euc2004_sjis2004.c'],
  'euc_cn_and_mic': ['euc_cn_and_mic/euc_cn_and_mic.c'],
  'euc_jp_and_sjis': ['euc_jp_and_sjis/euc_jp_and_sjis.c'],
  'euc_kr_and_mic': ['euc_kr_and_mic/euc_kr_and_mic.c'],
  'euc_tw_and_big5': [
    'euc_tw_and_big5/euc_tw_and_big5.c',
    'euc_tw_and_big5/big5.c',
  ],
  'latin2_and_win1250': ['latin2_and_win1250/latin2_and_win1250.c'],
  'latin_and_mic': ['latin_and_mic/latin_and_mic.c'],
  'utf8_and_big5': ['utf8_and_big5/utf8_and_big5.c'],
  'utf8_and_cyrillic': ['utf8_and_cyrillic/utf8_and_cyrillic.c'],
  'utf8_and_euc2004': ['utf8_and_euc2004/utf8_and_euc2004.c'],
  'utf8_and_euc_cn': ['utf8_and_euc_cn/utf8_and_euc_cn.c'],
  'utf8_and_euc_jp': ['utf8_and_euc_jp/utf8_and_euc_jp.c'],
  'utf8_and_euc_kr': ['utf8_and_euc_kr/utf8_and_euc_kr.c'],
  'utf8_and_euc_tw': ['utf8_and_euc_tw/utf8_and_euc_tw.c'],
  'utf8_and_gb18030': ['utf8_and_gb18030/utf8_and_gb18030.c'],
  'utf8_and_gbk': ['utf8_and_gbk/utf8_and_gbk.c'],
  'utf8_and_iso8859': ['utf8_and_iso8859/utf8_and_iso8859.c'],
  'utf8_and_iso8859_1': ['utf8_and_iso8859_1/utf8_and_iso8859_1.c'],
  'utf8_and_johab': ['utf8_and_johab/utf8_and_johab.c'],
  'utf8_and_sjis': ['utf8_and_sjis/utf8_and_sjis.c'],
  'utf8_and_sjis2004': ['utf8_and_sjis2004/utf8_and_sjis2004.c'],
  'utf8_and_uhc': ['utf8_and_uhc/utf8_and_uhc.c'],
  'utf8_and_win': ['utf8_and_win/utf8_and_win.c'],
}

foreach encoding, sources : encodings
  source_files = files(sources)

  if host_system == 'windows'
    source_files += rc_lib_gen.process(win32ver_rc, extra_args: [
      '--NAME', encoding])
  endif

  backend_targets += shared_module(encoding,
    source_files,
    kwargs: pg_mod_args,
  )
endforeach
