# Copyright (c) 2024-2025, PostgreSQL Global Development Group

test_dsm_registry_sources = files(
  'test_dsm_registry.c',
)

if host_system == 'windows'
  test_dsm_registry_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_dsm_registry',
    '--FILEDESC', 'test_dsm_registry - test code for the DSM registry',])
endif

test_dsm_registry = shared_module('test_dsm_registry',
  test_dsm_registry_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_dsm_registry

test_install_data += files(
  'test_dsm_registry.control',
  'test_dsm_registry--1.0.sql',
)

tests += {
  'name': 'test_dsm_registry',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_dsm_registry',
    ],
  },
}
