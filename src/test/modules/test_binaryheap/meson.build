# Copyright (c) 2025, PostgreSQL Global Development Group

test_binaryheap_sources = files(
  'test_binaryheap.c',
)

if host_system == 'windows'
  test_binaryheap_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_binaryheap',
    '--FILEDESC', 'test_binaryheap - test code for binaryheap',])
endif

test_binaryheap = shared_module('test_binaryheap',
  test_binaryheap_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_binaryheap

test_install_data += files(
  'test_binaryheap.control',
  'test_binaryheap--1.0.sql',
)

tests += {
  'name': 'test_binaryheap',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_binaryheap',
    ],
  },
}
