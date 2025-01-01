# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_copy_callbacks_sources = files(
  'test_copy_callbacks.c',
)

if host_system == 'windows'
  test_copy_callbacks_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_copy_callbacks',
    '--FILEDESC', 'test_copy_callbacks - test COPY callbacks',])
endif

test_copy_callbacks = shared_module('test_copy_callbacks',
  test_copy_callbacks_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_copy_callbacks

test_install_data += files(
  'test_copy_callbacks.control',
  'test_copy_callbacks--1.0.sql',
)

tests += {
  'name': 'test_copy_callbacks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_copy_callbacks',
    ],
  },
}
