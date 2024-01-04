# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
  'slot.c',
  'slotfuncs.c',
  'syncrep.c',
  'walreceiver.c',
  'walreceiverfuncs.c',
  'walsender.c',
)

# see ../parser/meson.build
repl_parser_sources = []

repl_scanner = custom_target('repl_scanner',
  input: 'repl_scanner.l',
  output: 'repl_scanner.c',
  command: flex_cmd,
)
generated_sources += repl_scanner
repl_parser_sources += repl_scanner

repl_gram = custom_target('repl_gram',
  input: 'repl_gram.y',
  kwargs: bison_kw,
)
generated_sources += repl_gram.to_list()
repl_parser_sources += repl_gram

syncrep_scanner = custom_target('syncrep_scanner',
  input: 'syncrep_scanner.l',
  output: 'syncrep_scanner.c',
  command: flex_cmd,
)
generated_sources += syncrep_scanner
repl_parser_sources += syncrep_scanner

syncrep_gram = custom_target('syncrep_gram',
  input: 'syncrep_gram.y',
  kwargs: bison_kw,
)
generated_sources += syncrep_gram.to_list()
repl_parser_sources += syncrep_gram

repl_parser = static_library('repl_parser',
  repl_parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += repl_parser

subdir('logical')
