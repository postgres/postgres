# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tsm_system_time_sources = files(
  'tsm_system_time.c',
)

if host_system == 'windows'
  tsm_system_time_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'tsm_system_time',
    '--FILEDESC', 'tsm_system_time - TABLESAMPLE method which accepts time in milliseconds as a limit',])
endif

tsm_system_time = shared_module('tsm_system_time',
  tsm_system_time_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += tsm_system_time

install_data(
  'tsm_system_time--1.0.sql',
  'tsm_system_time.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tsm_system_time',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'tsm_system_time',
    ],
  },
}
