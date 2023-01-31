# Copyright (c) 2022-2023, PostgreSQL Global Development Group

backend_sources += files(
  'dependencies.c',
  'extended_stats.c',
  'mcv.c',
  'mvdistinct.c',
)

ext_stats_parser_sources = []

statistics_scanner = custom_target('statistics_scanner',
  input: 'statistics_scanner.l',
  output: 'statistics_scanner.c',
  command: flex_cmd,
)
generated_sources += statistics_scanner
ext_stats_parser_sources += statistics_scanner

statistics_gram = custom_target('statistics_gram',
  input: 'statistics_gram.y',
  kwargs: bison_kw,
)
generated_sources += statistics_gram.to_list()
ext_stats_parser_sources += statistics_gram

stats_parser = static_library('stats_parser',
  ext_stats_parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += stats_parser
