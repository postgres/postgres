# Copyright (c) 2025-2026, PostgreSQL Global Development Group

test_lwlock_tranches_sources = files(
  'test_lwlock_tranches.c',
)

if host_system == 'windows'
  test_lwlock_tranches_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_lwlock_tranches',
    '--FILEDESC', 'test_lwlock_tranches - test code for LWLock tranches allocated by extensions',])
endif

test_lwlock_tranches = shared_module('test_lwlock_tranches',
  test_lwlock_tranches_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_lwlock_tranches

test_install_data += files(
  'test_lwlock_tranches.control',
  'test_lwlock_tranches--1.0.sql',
)

tests += {
  'name': 'test_lwlock_tranches',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_lwlock_tranches',
    ],
    'regress_args': ['--temp-config', files('test_lwlock_tranches.conf')],
    'runningcheck': false,
  },
}
