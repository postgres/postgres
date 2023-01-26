# Copyright (c) 2022-2023, PostgreSQL Global Development Group

backend_sources += files(
  'dependencies.c',
  'extended_stats.c',
  'mcv.c',
  'mvdistinct.c',
)

stats_scan = custom_target('statsscan',
  input: 'statistics_scanner.l',
  output: 'statistics_scanner.c',
  command: flex_cmd,
)
generated_sources += stats_scan
backend_sources += stats_scan

stats_gram = custom_target('stats_gram',
  input: 'statistics_gram.y',
  kwargs: bison_kw,
)
generated_sources += stats_gram.to_list()
backend_sources += stats_gram