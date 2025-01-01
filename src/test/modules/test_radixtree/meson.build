# Copyright (c) 2024-2025, PostgreSQL Global Development Group

test_radixtree_sources = files(
  'test_radixtree.c',
)

if host_system == 'windows'
  test_radixtree_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_radixtree',
    '--FILEDESC', 'test_radixtree - test code for src/include/lib/radixtree.h',])
endif

test_radixtree = shared_module('test_radixtree',
  test_radixtree_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_radixtree

test_install_data += files(
  'test_radixtree.control',
  'test_radixtree--1.0.sql',
)

tests += {
  'name': 'test_radixtree',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_radixtree',
    ],
  },
}
