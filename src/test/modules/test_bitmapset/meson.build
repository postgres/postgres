# Copyright (c) 2025-2026, PostgreSQL Global Development Group

test_bitmapset_sources = files(
  'test_bitmapset.c',
)

if host_system == 'windows'
  test_bitmapset_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_bitmapset',
    '--FILEDESC', 'test_bitmapset - test code for src/include/nodes/bitmapset.h',])
endif

test_bitmapset = shared_module('test_bitmapset',
  test_bitmapset_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_bitmapset

test_install_data += files(
  'test_bitmapset.control',
  'test_bitmapset--1.0.sql',
)

tests += {
  'name': 'test_bitmapset',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_bitmapset',
    ],
  },
}
