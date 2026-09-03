# Copyright (c) 2025-2026, PostgreSQL Global Development Group

test_indexscan_sources = files(
  'test_indexscan.c',
)

if host_system == 'windows'
  test_indexscan_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_indexscan',
    '--FILEDESC', 'test_indexscan - test index scan internals',])
endif

test_indexscan = shared_module('test_indexscan',
  test_indexscan_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_indexscan

test_install_data += files(
  'test_indexscan.control',
  'test_indexscan--1.0.sql',
)

tests += {
  'name': 'index',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'hot_chain',
    ],
  },
  'isolation': {
    'specs': [
      'killtuples',
    ],
  },
}
