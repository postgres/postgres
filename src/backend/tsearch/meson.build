# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
  'dict.c',
  'dict_ispell.c',
  'dict_simple.c',
  'dict_synonym.c',
  'dict_thesaurus.c',
  'regis.c',
  'spell.c',
  'to_tsany.c',
  'ts_locale.c',
  'ts_parse.c',
  'ts_selfuncs.c',
  'ts_typanalyze.c',
  'ts_utils.c',
  'wparser.c',
  'wparser_def.c',
)

install_subdir('dicts',
  install_dir: dir_data / 'tsearch_data',
  strip_directory: true)
