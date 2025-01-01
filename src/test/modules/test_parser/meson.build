# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_parser_sources = files(
  'test_parser.c',
)

if host_system == 'windows'
  test_parser_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_parser',
    '--FILEDESC', 'test_parser - example of a custom parser for full-text search',])
endif

test_parser = shared_module('test_parser',
  test_parser_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_parser

test_install_data += files(
  'test_parser.control',
  'test_parser--1.0.sql',
)

tests += {
  'name': 'test_parser',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_parser',
    ],
  },
}
