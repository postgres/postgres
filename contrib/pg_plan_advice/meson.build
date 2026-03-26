# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_plan_advice_sources = files(
  'pg_plan_advice.c',
  'pgpa_ast.c',
  'pgpa_identifier.c',
  'pgpa_join.c',
  'pgpa_output.c',
  'pgpa_planner.c',
  'pgpa_scan.c',
  'pgpa_trove.c',
  'pgpa_walker.c',
)

pgpa_scanner = custom_target('pgpa_scanner',
  input: 'pgpa_scanner.l',
  output: 'pgpa_scanner.c',
  command: flex_cmd,
)
generated_sources += pgpa_scanner
pg_plan_advice_sources += pgpa_scanner

pgpa_parser = custom_target('pgpa_parser',
  input: 'pgpa_parser.y',
  kwargs: bison_kw,
)
generated_sources += pgpa_parser.to_list()
pg_plan_advice_sources += pgpa_parser

if host_system == 'windows'
  pg_plan_advice_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_plan_advice',
    '--FILEDESC', 'pg_plan_advice - help the planner get the right plan',])
endif

pg_plan_advice_inc = include_directories('.')

pg_plan_advice = shared_module('pg_plan_advice',
  pg_plan_advice_sources,
  include_directories: pg_plan_advice_inc,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_plan_advice

install_headers(
  'pg_plan_advice.h',
  install_dir: dir_include_server / 'contrib' / 'pg_plan_advice',
)

tests += {
  'name': 'pg_plan_advice',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'alternatives',
      'gather',
      'join_order',
      'join_strategy',
      'partitionwise',
      'prepared',
      'scan',
      'semijoin',
      'syntax',
    ],
  },
}
