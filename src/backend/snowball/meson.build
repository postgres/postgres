# Copyright (c) 2022-2024, PostgreSQL Global Development Group

dict_snowball_sources = files(
  'libstemmer/api.c',
  'libstemmer/utilities.c',
  'dict_snowball.c',
)

dict_snowball_sources += files(
  'libstemmer/stem_ISO_8859_1_basque.c',
  'libstemmer/stem_ISO_8859_1_catalan.c',
  'libstemmer/stem_ISO_8859_1_danish.c',
  'libstemmer/stem_ISO_8859_1_dutch.c',
  'libstemmer/stem_ISO_8859_1_english.c',
  'libstemmer/stem_ISO_8859_1_finnish.c',
  'libstemmer/stem_ISO_8859_1_french.c',
  'libstemmer/stem_ISO_8859_1_german.c',
  'libstemmer/stem_ISO_8859_1_indonesian.c',
  'libstemmer/stem_ISO_8859_1_irish.c',
  'libstemmer/stem_ISO_8859_1_italian.c',
  'libstemmer/stem_ISO_8859_1_norwegian.c',
  'libstemmer/stem_ISO_8859_1_porter.c',
  'libstemmer/stem_ISO_8859_1_portuguese.c',
  'libstemmer/stem_ISO_8859_1_spanish.c',
  'libstemmer/stem_ISO_8859_1_swedish.c',
  'libstemmer/stem_ISO_8859_2_hungarian.c',
  'libstemmer/stem_ISO_8859_2_romanian.c',
  'libstemmer/stem_KOI8_R_russian.c',
  'libstemmer/stem_UTF_8_arabic.c',
  'libstemmer/stem_UTF_8_armenian.c',
  'libstemmer/stem_UTF_8_basque.c',
  'libstemmer/stem_UTF_8_catalan.c',
  'libstemmer/stem_UTF_8_danish.c',
  'libstemmer/stem_UTF_8_dutch.c',
  'libstemmer/stem_UTF_8_english.c',
  'libstemmer/stem_UTF_8_finnish.c',
  'libstemmer/stem_UTF_8_french.c',
  'libstemmer/stem_UTF_8_german.c',
  'libstemmer/stem_UTF_8_greek.c',
  'libstemmer/stem_UTF_8_hindi.c',
  'libstemmer/stem_UTF_8_hungarian.c',
  'libstemmer/stem_UTF_8_indonesian.c',
  'libstemmer/stem_UTF_8_irish.c',
  'libstemmer/stem_UTF_8_italian.c',
  'libstemmer/stem_UTF_8_lithuanian.c',
  'libstemmer/stem_UTF_8_nepali.c',
  'libstemmer/stem_UTF_8_norwegian.c',
  'libstemmer/stem_UTF_8_porter.c',
  'libstemmer/stem_UTF_8_portuguese.c',
  'libstemmer/stem_UTF_8_romanian.c',
  'libstemmer/stem_UTF_8_russian.c',
  'libstemmer/stem_UTF_8_serbian.c',
  'libstemmer/stem_UTF_8_spanish.c',
  'libstemmer/stem_UTF_8_swedish.c',
  'libstemmer/stem_UTF_8_tamil.c',
  'libstemmer/stem_UTF_8_turkish.c',
  'libstemmer/stem_UTF_8_yiddish.c',
)

# see comment in src/include/snowball/header.h
stemmer_inc = include_directories('../../include/snowball')

if host_system == 'windows'
  dict_snowball_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dict_snowball',
    '--FILEDESC', 'snowball - natural language stemmers',])
endif

dict_snowball = shared_module('dict_snowball',
  dict_snowball_sources,
  c_pch: pch_postgres_h,
  kwargs: pg_mod_args + {
    'include_directories': [stemmer_inc],
  }
)

snowball_create = custom_target('snowball_create',
  output: ['snowball_create.sql'],
  depfile: 'snowball_create.dep',
  command: [
    perl, files('snowball_create.pl'),
    '--depfile',
    '--input', '@CURRENT_SOURCE_DIR@',
    '--outdir', '@OUTDIR@',
  ],
  install: true,
  install_dir: dir_data,
)
generated_sources += snowball_create

install_subdir('stopwords',
  install_dir: dir_data / 'tsearch_data',
  strip_directory: true,
)

backend_targets += dict_snowball
backend_targets += snowball_create
