# Copyright (c) 2022-2023, PostgreSQL Global Development Group

basic_wal_module_sources = files(
  'basic_wal_module.c',
)

if host_system == 'windows'
  basic_wal_module_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'basic_wal_module',
    '--FILEDESC', 'basic_wal_module - basic write-ahead log module',])
endif

basic_wal_module = shared_module('basic_wal_module',
  basic_wal_module_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += basic_wal_module

tests += {
  'name': 'basic_wal_module',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'basic_wal_module',
    ],
    'regress_args': [
      '--temp-config', files('basic_wal_module.conf'),
    ],
    # Disabled because these tests require "shared_preload_libraries=basic_wal_module",
    # which typical runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
}
