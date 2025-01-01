# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_install_data += files(
  'test_pg_dump.control',
  'test_pg_dump--1.0.sql',
)

tests += {
  'name': 'test_pg_dump',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_pg_dump',
    ],
  },
  'tap': {
    'tests': [
      't/001_base.pl',
    ],
  },
}
